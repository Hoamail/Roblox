# P0 — Static RE of `velext_v1.exe` (REA / Ghidra 12.1.3)

Target: `C:\Users\adminn\ext_project\velext_v1.exe`
sha256: `0a78225ff73d6c93f41b2eebc747c701610ce26c10962ac94de58eb02d29f94c`
Size 5,093,888 B — PE x86-64, image base `0x140000000`
Sections: `.text` 0x140001000/0x2576B3 · `.rdata` 0x140259000/0xD3656 · `.data` 0x14032D000/0x12816 ·
`.pdata` 0x140340000 · `.rsrc` 0x14035D000/0x18E3A8 · `.reloc` 0x1404EC000
Ghidra: 5300 procedures, 2082 strings.
VA↔file offset: `.text` file = VA − 0x140000C00 · `.rdata` file = VA − 0x140000000 + 0x257C00 − 0x140259000 + … (xem bảng section).
All findings below are **static**; nothing was executed.

---

## 1. Kết luận trước (TL;DR)

1. **velext không import một API memory nào** (không `ReadProcessMemory`, không `WriteProcessMemory`, không `NtReadVirtualMemory`, không `VirtualAlloc/Protect`, không `OpenProcess`). Import chỉ có: `WININET`, `DWMAPI`, `USER32/IMM32`, `KERNEL32` (LoadLibraryA, GetProcAddress, GetCurrentProcess, IsDebuggerPresent, RtlCaptureContext, RtlVirtualUnwind, RtlLookupFunctionEntry, QueryPerformanceCounter, critical-section…), CRT + MSVCP140.
2. Nó đọc/ghi memory Roblox bằng **direct syscall** (`syscall` instruction trong `.text`, không qua stub ntdll):
   - `FUN_14022fd20` (VA `0x14022fd20`, 10 byte): `MOV qword ptr GS:[0x16B4], RCX ; RET` → nạp syscall id.
   - `FUN_14022fd2a` (VA `0x14022fd2a`, 14 byte): `MOV EAX, dword ptr GS:[0x16B4] ; MOV R10, RCX ; SYSCALL ; RET` → thực thi.
   - 561 call-site (282 nạp id + 279 gọi stub) — mọi I/O memory đi qua đây.
3. Syscall id được **resolve lúc runtime từ ntdll** (không hardcode, không có tên API dạng plaintext):
   - `FUN_1400636e0(name)` → base module bằng cách đi bộ **PEB Ldr** (`gs:[0x60] → +0x18 → InMemoryOrderLinks +0x20`), so tên kiểu case-insensitive (`c|0x20`).
   - `FUN_140093320(name, base)` → parse PE export, so tên, rồi đọc `mov eax, <imm32>` tại **stub+4** ⇒ đúng kiểu **Hell's Gate**; nếu stub bị hook thì quét stub lân cận ±0x20 byte và bù `−delta` ⇒ **Halo's Gate**.
4. Syscall id cache trong `.data` (giá trị `−1` = chưa resolve). Xác định được 2 cái quan trọng:
   - `DAT_140332a50` = **NtReadVirtualMemory** (147 call-site; wrapper `FUN_1400a3e50(handle, addr) -> u64`).
   - `DAT_140332a20` = **NtWriteVirtualMemory** (60 call-site; dạng gọi 4–5 tham số `(handle, addr, src, size[, &written])`, buffer nguồn được ghi **trước** lời gọi, ví dụ ghi double `0.001` = `0x3f1a36e2eb1c432d`).
   - Nhóm khác: `a18`, `a1c`(4), `a30`(5), `d60`(6), `d6c`(6), `d70`(2), `d80`(8), `d98`(7), `dac`, `dc0`, `df0`, `e88`, `ed4`, `edc`, `f3c` (rất có thể là NtAllocate/NtProtect/NtQueryVirtualMemory/NtQuerySystemInformation/NtClose…).
5. **Offset Roblox không hề hardcode** trong file. Chúng nằm trong bảng `.data` `0x14032D000–0x14032D900` dạng slot
   8 byte, giá trị ban đầu `0xDEADBEEF` (−559038737), được **điền lúc runtime**.
   Code dùng chúng kiểu `base + DAT_14032dXXX`, ví dụ trong function gắn với Humanoid:
   - `FUN_14009bb90(param_2, DAT_14032d1f8)`
   - `FUN_1400d1b90(param_1, DAT_14032d1e0 + lVar18, &local_218)`
   - `param_2 = DAT_14032d270 + param_2` (trong `FUN_1400d96b0`)
   ⇒ Không thể đọc offset trong file tĩnh; **phải đọc lúc runtime** (P1).
6. Truy cập property theo **tên**, không theo offset cứng: 2 function lớn so tên class
   (`"Player"`, `"Camera"`, `"Model"`, `"Humanoid"` — movabs `0x64696f6e616d7548` tại `0x1400cde81` và `0x14011ab2d`)
   và dùng chuỗi property `Velocity`, `Position`, `WalkSpeed`, `JumpPower`, `MoveDirection`, `CFrame`, `Health`, `MaxHealth`, `Team`, `Name`, `Text`, `Brightness`.
   → `FUN_1400cd7d0` (5.193 B, VA 0x1400cd7d0) và `FUN_14011a670` (7.168 B, VA 0x14011a670) = inspector/property bridge.
7. **Đính chính kết quả scan trước:** các hit `MOV [RCX+0x1CC], imm32` (VA `0x140176d54`) và `MOVUPS [RCX+0x3A4], XMM0` (VA `0x1401772e4`) là **false positive** — chúng nằm trong `FUN_140176c10` (2.722 B), một bảng hằng float/matrix (0.29, 0.48, 0.54, 0.26, 0.59, 0.98, …) dùng cho toán 3D, **không phải** Humanoid.
   ⇒ Trong `velext_v1.exe` **không có** bằng chứng nào của việc ghi thẳng `Humanoid+0x1CC` / `+0x3A4`. Giả thuyết H1 (“velext ghi cặp WalkSpeed+WalkspeedCheck”) **không được xác nhận** bằng static analysis.
8. Có HTTP client (`InternetOpenA/InternetConnectA/InternetCrackUrlA/InternetReadFile`) + chuỗi `"http://"`, `"GET"`, nhưng **không tìm thấy URL plaintext** nào (không `offsets`, `.json`, `imtheo`, `roblox.com`, `.lol`). ⇒ URL (nếu có) được build/giải mã runtime.
9. Không có file PE/MZ nhúng trong `.rsrc`, không có chuỗi AOB-signature dạng `"48 8B ?? ??"` tìm thấy trong `.rdata` ⇒ khả năng cao offset đến từ (a) AOB scan dạng mã hoá, (b) HTTP feed, hoặc (c) resolve từ chính reflection data của Roblox — **cần xác minh ở P1**.
10. Tên API/URL/offset không tồn tại plaintext trong file ⇒ có lớp “string decode” runtime. Ví dụ `FUN_140024210`
    (VA `0x140024210`): giải mã **5 byte** từ `0x1402a4900` → buffer `0x14033b144`, cache con trỏ ở `0x140331a78`
    (keystream splitmix64: C1=0xA5A5A5A5A5A5A5A5, C2=0x816CEB9FA7C60CF8, C3=0xBF586D6D1CE4E5B9,
    C4=0x94D049BB133111EB, C5=0x9E3779B97F4A7C15; `out = ((ROR8(((u>>24)+raw ^ (u>>16)), (u>>32)&7)) - (u>>8)) ^ (u&0xFF)`).
    **Đã mô phỏng lại đúng theo asm nhưng kết quả không ra ASCII** ⇒ còn thiếu một lớp (nhiều khả năng blob tại `0x1402a4900` không phải tên, hoặc còn bước biến đổi thứ 2). **Open item.**

---

## 2. Bằng chứng chính (byte-level)

### 2.1 Syscall dispatcher
```
0x14022fd20: 65 48 89 0C 25 B4 16 00 00 C3        ; MOV qword ptr GS:[0x16B4], RCX ; RET
0x14022fd2a: 65 8B 04 25 B4 16 00 00 4C 8B D1 0F 05 C3
             ; MOV EAX, dword ptr GS:[0x16B4] ; MOV R10, RCX ; SYSCALL ; RET
```

### 2.2 Resolve syscall number (Hell's/Halo's Gate) — `FUN_140093320`
- Duyệt `NumberOfNames` trong export directory; so tên (fold `c+0x20`).
- Lấy RVA hàm theo `AddressOfNameOrdinals`/`AddressOfFunctions`; kiểm tra `SizeOfImage` (chống trỏ ra ngoài).
- Kiểm tra stub: bytes `4C 8B D1` (`mov r10, rcx`) — biểu diễn trong pseudo là `'L', -0x75, -0x2f, -0x48`.
- **Nếu khớp:** `return *(int*)(stub+4)` (imm32 của `mov eax, imm32`) = syscall id.
- **Nếu không khớp (bị hook):** quét `stub + i*0x20` và `stub − i*0x20`, `i ≤ 0x1FF`, trả `*(int*)(neighbor+4) ∓ i`.

### 2.3 Getter id-syscall (`0x140050aa0 … 0x140051xxx`, mỗi hàm 96 byte)
```c
void FUN_140051330(void) {                    // = get id NtReadVirtualMemory
  uVar1 = DAT_1403324a0;                      // con trỏ tên API (set runtime)
  if (FUN_1400636e0(DAT_140332818) == 0) FUN_140063ab0(DAT_140332818);
  if (DAT_140335190 == 0) DAT_140335190 = FUN_1400636e0(DAT_140332818);  // base ntdll (cache)
  DAT_140332a50 = FUN_140093320(uVar1, DAT_140335190);
}
```

### 2.4 Read/Write primitive
```c
// READ (DAT_140332a50) — FUN_1400a3e50
undefined8 FUN_1400a3e50(longlong *h, undefined8 addr) {
  lVar1 = *h; local_res18[0] = 0; local_res8 = 0;
  if (lVar1 != 0 && DAT_140332a50 != -1) { FUN_14022fd20(DAT_140332a50);
      FUN_14022fd2a(lVar1, addr, local_res18, 8, &local_res8); return local_res18[0]; }
  return 0; }

// WRITE (DAT_140332a20) — ví dụ trong FUN_140096140
local_708 = 0x3f1a36e2eb1c432d;               // double 0.001
FUN_14022fd20(DAT_140332a20);
FUN_14022fd2a(uVar43, lVar59, &local_708, 8); // handle, addr, src, size
// và: FUN_14022fd2a(local_6d8, local_7a8, plVar40, 8);   (src = con trỏ đã dựng trước)
```

### 2.5 Bảng offset `.data` (0xDEADBEEF placeholder)
```
0x14032d1b0: 0 0 0 0 0 0 15 0 1 240 240 60
0x14032d1e0: -559038737 0 -559038737 0 -559038737 0 ...   (toàn slot 0xDEADBEEF)
0x14032d390: -559038737 0 | -559038737 0 | 1076522384 1 | 1076522352 1 | 1076513208 1 | ...
```
(`1076522384 = 0x40268C90` ⇒ cặp `(ptr 0x140268C90, flags=1)` — entry con trỏ chuỗi; slot `0xDEADBEEF` = offset chờ điền.)

Hằng config đáng chú ý trong `.data`: `1065353216`=1.0f, `1084227584`=5.0f, `1098907648`=16.0f, `1112014848`=50.0f, `1114636288`=60.0f, `1116471296`=70.0f.

---

## 3. Ý nghĩa cho `ext_overlay.exe` + Error 268

- Overlay của mình đi qua `WriteProcessMemory` (kernel32 → ntdll). Hyperion (Byfron) hook/kiểm tra đường này; velext **né tầng hook usermode** bằng direct syscall ⇒ khác biệt kỹ thuật lớn nhất giữa 2 tool.
- Tuy nhiên **268 không phải do "dùng API gì"**: 268 là kick phía Hyperion khi phát hiện bất nhất/tamper (integrity check, guard page…). Direct syscall chỉ giúp **giảm khả năng bị phát hiện qua hook**, không sửa được lỗi logic khi ghi `WalkSpeed` mà không cập nhật field “check”.
- Trong binary không có bằng chứng velext ghi `Humanoid+0x1CC`/`+0x3A4`. Velext dùng **bảng offset điền runtime** ⇒ muốn biết velext ghi gì, phải **đọc bảng đó lúc chạy** hoặc trace syscall thật (P1/P2).

## 4. Open items + bước P1

1. **Đọc runtime `.data` offset của velext**: attach/đọc process `velext_v1.exe` đang chạy, dump
   `0x14032D000..0x14032D900` → map slot `DAT_14032dXXX` ↔ offset Roblox thật (kể cả check field).
   (Chỉ đọc memory, không inject; vẫn cần approval vì là đọc process khác.)
2. **Xác định nguồn offset**: (a) AOB scan / (b) HTTP feed / (c) reflection Roblox.
   Cách phân biệt: log network (URL/IP) khi velext khởi động + xem `.data` được ghi bởi hàm nào (đã có danh sách 195 writer site).
3. **Hoàn thiện decoder tên API** (`FUN_140024210` + 7 site khác: `0x14000e086`, `0x140018d06`, `0x14001a986`, `0x14001bd06`, `0x140040906`, `0x140043186`, `0x14004cf06`) để có full danh sách syscall.
4. **`src/wpmtrace.cpp`** (như trong `PLAN_speed_jump.md`): log mọi `NtWriteVirtualMemory` (hoặc BP syscall) với `base/size/hex` khi chạy velext → biết chính xác nó ghi offset nào, cadence bao nhiêu.
5. Chốt account alt + place baseplate cho T0–T10.
