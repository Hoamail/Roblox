# HANDOFF — S1: tách menu overlay thành tab files

**Ngày:** 2026-10-20
**Trạng thái:** PASS toàn bộ gate tự động (S1.0 → S1.14). Còn checklist in-game (§7).

---

## 1. Kết quả đo được

| Hạng mục | Trước S1 | Sau S1 | Ghi chú |
|---|---|---|---|
| `src\overlay.cpp` | 7.217 dòng | **6.123 dòng** | −1.094 dòng |
| File nguồn overlay | 2 | **11** | overlay.cpp + offsets.cpp + 9 file `ovl\` |
| Tổng `src\ovl\` | 0 | **13 file / 1.381 dòng** | 4 header + ovl_ui.cpp + 8 tab |
| Body tab move | — | **792 dòng verbatim** | 8/8 khớp nguyên văn backup |
| `ext_overlay.exe` | 7.775.980 B | 8.868.676 B | +1,09 MB do 11 TU × inline + DWARF; `.text` thực tăng nhỏ |
| `build_fast.bat` | exit 0 (giả) | exit 0 (thật) | sửa echo + `exit /b` |
| warning | 0 | **0** | |
| `-aimtest` | 0 FAIL | **0 FAIL** | |
| Menu hits 8 tab | baseline | **8/8 khớp** | hits + size tuyệt đối |
| Build từ `C:\Windows\System32` | — | **OK** | nhờ `cd /d "%~dp0"` |
| Fail-injection (rename 1 .cpp) | — | **exit 1** | cổng build giờ chặn thật |

### Hits baseline vs final (`backup\s1_baseline_hits.txt` vs `backup\s1_hits_final_artifact.txt`; `s1_hits_final.txt` là bản trước đó, cùng số)

| Tab | Tên | hits | size |
|---|---|---|---|
| 0 | Body | 17 | 344,253 |
| 1 | Combat | 15 | 380,236 |
| 2 | Visual | 28 | 344,525 |
| 3 | Teleport | 17 | 344,500 |
| 4 | World | 25 | 344,455 |
| 5 | Server | 23 | 344,455 |
| 6 | Tools | 10 | 344,355 |
| 7 | Misc | 32 | 344,525 |

Cả 8 tab: exit 0, `collapsed=0`, `pos=928,173` — khớp baseline từng byte số.

---

## 2. Cây file sau S1

```
src\
  overlay.cpp        6.123   khung menu, tab buttons, footer, hit-test popup,
                              main loop, aim, ESP, entity cache, server worker, cfg IO
  ovl\
    ovl_types.h        76     HitRect, Waypoint, Ent, SrvRow, SrvJoin, SrvState, SrvWire
    ovl_ui.h           30     prototype helper UI + extern toast/capture/hits
    ovl_ui.cpp        199     hitMark, parseKey, chkInt×2, slider×2, keyName,
                              keyStateLabel, keyBindWidget, markChildArea, toast,
                              g_menuHits, g_toast/tick, g_cap*
    ovl_state.h       121     extern 85 cfg/state + srv + w* + kColorPal +
                              velextAlphaFromDt + saveWaypoints + 12 prototype server/profile
    ovl_tabs.h         33     MenuCtx + 8 prototype drawTab*
    tab_body.cpp      114     drawTabBody      (body 97 dòng)
    tab_combat.cpp     70     drawTabCombat    (53)
    tab_visual.cpp    108     drawTabVisual    (91)
    tab_teleport.cpp  209     drawTabTeleport  (189)
    tab_world.cpp     109     drawTabWorld     (92)
    tab_server.cpp    181     drawTabServer    (164)
    tab_tools.cpp      14     drawTabTools     (6)
    tab_misc.cpp      117     drawTabMisc      (100)
```

Dispatch trong `overlay.cpp` (8 dòng, ngay trước marker `// ---- status footer`):

```cpp
if      (cfgMenuTab == 0) drawTabBody(tabCtx);
else if (cfgMenuTab == 1) drawTabCombat(tabCtx);
else if (cfgMenuTab == 2) drawTabVisual(tabCtx);
else if (cfgMenuTab == 3) drawTabTeleport(tabCtx);
else if (cfgMenuTab == 4) drawTabWorld(tabCtx);
else if (cfgMenuTab == 5) drawTabServer(tabCtx);
else if (cfgMenuTab == 6) drawTabTools(tabCtx);
else                      drawTabMisc(tabCtx);
```

Tab buttons (`bool act = (cfgMenuTab == t);`) vẫn nằm trong `overlay.cpp` — không đổi thứ tự tab.

---

## 3. MenuCtx + alias pattern (quan trọng khi sửa tab)

`MenuCtx` (trong `ovl_tabs.h`):

```cpp
struct MenuCtx {
    Memory& m;
    const Offsets& o;
    std::vector<Ent>& ents;   // non-const: Teleport body dùng `Ent& e = ents[i]`
    uint64_t place = 0;
    bool haveWrite = false;
    bool aimOn = false, trigOn = false, acOn = false;
    bool flyApply = false, speedApply = false, jumpApply = false;
    int dbgEnts = 0;
    float loopMs = 0.0f;
    DWORD nowTick = 0;        // = nowLoop
};
```

Mỗi tab file mở đầu bằng alias **cùng tên** với biến main-local cũ:

```cpp
Memory& m = c.m;
const Offsets& o = c.o;
std::vector<Ent>& ents = c.ents;
uint64_t place = c.place;
bool haveWrite = c.haveWrite;
bool aimOn = c.aimOn, trigOn = c.trigOn, acOn = c.acOn;
bool flyApply = c.flyApply, speedApply = c.speedApply, jumpApply = c.jumpApply;
int dbgEnts = c.dbgEnts;
float loopMs = c.loopMs;
DWORD nowLoop = c.nowTick;
```

Nhờ vậy **thân tab giữ nguyên 100% văn bản** (đã chứng minh bằng script: 792/792 dòng body xuất hiện nguyên văn trong `backup\overlay_S1PRE_20261020.cpp`). Không rename identifier, không sửa logic.

Lưu ý ngữ nghĩa alias:
- `m`, `o`, `ents` là reference → thao tác trên object thật.
- Các `bool/int/float/nowLoop` là **bản copy tại thời điểm dispatch** — đúng bằng giá trị inline code cũ đọc trong frame đó. Tab không được ghi ngược lại các biến này (hiện không tab nào làm vậy).
- `nowLoop` trong tab = `c.nowTick` = `GetTickCount()` đầu frame, y hệt trước.

---

## 4. Thay đổi ngoài việc move

### 4.1 C1 — dead code trong overlay.cpp (đã verify)
- 64 dòng xoá + 2 dòng thay thế; 7.217 → **7.153 dòng**.
- Danh sách dòng gốc: `backup\C1_removed_lines.txt`.
- Gồm G1 (lastAimTick/vAim*/drawnGate/smAim*/shown*/dbgTgt), G2 (g_hwnd/g_plyCurJumpH/g_menuWin*/cfgHitboxPlace/hitboxShown...), G3 (`Ent.uid`, `AimTarget.fx/fy`, `SrvRow.fps`, `SrvState.creatorId` + block creator-id, `cfgAimIgnoreDead`, `cfgAimPark`).
- `AimTarget.score` **giữ nguyên** (còn đọc trong nhánh `cfgAimNearest==0`).
- Verify sau C1: build 0, aimtest 0, hits 8/8 khớp baseline (`backup\s1_hits_after_C1.txt`).

### 4.2 C2 — offsets.h / offsets.cpp (đã verify)
- Xoá 14 field `offsets.h` (13 field G4 + `dataModelCreatorId`) và 14 dòng mapping `offsets.cpp` (13 mapping + decl `cfx` chỉ phục vụ 3 field chết).
- Field list: `DEAD_CODE_AUDIT.md` §G4.
- Bằng chứng không mất code thật: `.text` chỉ −1.376 B; phần giảm còn lại là DWARF (`.debug_info` −20.725 B, `.debug_line` −4.618 B) — so `objdump -h` giữa `backup\ext_overlay_S1PRE_20261020.exe` và exe sau C2.
- Verify sau C2: full `build.bat` 0 lỗi, aimtest 0, hits 8/8 (`backup\s1_hits_after_C2.txt`).

### 4.3 C3 — dwmapi
- `#include <dwmapi.h>` đã bị bỏ **trong batch C1** (dòng 7). Build exit 0 ⇒ coi như C3 xong.
- Vẫn giữ `-ldwmapi` trong cả 2 build script (bắt buộc cho `lib\libimgui.a(imgui_impl_win32.o)`).

### 4.4 memory.h — fix multiple definition khi header được include bởi nhiều TU
Trước S1 chỉ có 1 TU include `memory.h`, nên asm toàn cục `ntWriteSyscall` không đụng ai. Khi tách 9 TU mới, linker báo `multiple definition of ntWriteSyscall`. Đã đổi sang **static naked function**:

```cpp
extern "C" {
__attribute__((used)) static uint32_t g_ntWriteId = 0;

__attribute__((naked)) static long ntWriteSyscall(void* handle, void* addr,
                                                  const void* buf, size_t size,
                                                  size_t* written) {
    __asm__(
        "movl g_ntWriteId(%rip), %eax\n"
        "movq %rcx, %r10\n"
        "syscall\n"
        "ret\n");
}
}
```

Hệ quả cần biết:
- Mỗi TU có bản `ntWriteSyscall` + `g_ntWriteId` riêng → không còn ODR collision.
- `writeBytes()` chỉ dùng syscall khi `directWrite && g_ntWriteId`; nếu TU đó chưa gọi `openWrite()` thì `g_ntWriteId=0` → **tự fallback `WriteProcessMemory`** (vẫn ghi đúng, chỉ mất stealth).
- Hiện mọi write đều phát sinh trong `overlay.cpp` (TU đã `openWrite()`), nên đường syscall vẫn như cũ. Nếu sau này muốn tab tự ghi memory bằng syscall trực tiếp, phải move `openWrite`/write về cùng TU hoặc tách `ntwrite.cpp` dùng chung.

### 4.5 Build scripts (bắt buộc sửa vì có 9 file mới)
`build_fast.bat` + `build.bat`:
- Danh sách nguồn overlay **giống hệt nhau** (đã diff): 11 file `.cpp` tường minh, không glob.
- Thêm `cd /d "%~dp0"` → chạy được từ CWD bất kỳ (test từ `C:\Windows\System32` OK).
- `build_fast.bat`: đưa `echo build overlay exit: %ERRORLEVEL%` ra **ngoài** block `if/else` (trước đây `%ERRORLEVEL%` bị expand lúc parse → luôn in 0).
- Thêm `exit /b %ERRORLEVEL%` cuối cả 2 script → exit code thật (đã test: rename 1 .cpp → exit 1; khôi phục → exit 0).
- `-ldwmapi` giữ nguyên.

### 4.6 Khác
- Comment `cfgMenuTab` trong overlay.cpp cập nhật đủ 8 tab (comment-only).
- `ovl_ui.cpp`: definition `keyBindWidget` bỏ default arg `= 150.0f` (default nằm ở header, tránh “redefinition of default argument”).

---

## 5. Bằng chứng verify

1. **Move verbatim**: script đọc từng `tab_*.cpp`, bỏ header/alias, so khối body với `backup\overlay_S1PRE_20261020.cpp` → **8/8 True**, tổng 792 dòng.
2. **Cân bằng ImGui child**: `BeginChild(` / `EndChild(` từng tab: 0/0, 0/0, 1/1, 2/2, 2/2, 1/1, 1/1, 1/1 → OK.
3. **Không còn thân tab trong overlay.cpp**: `cfgMenuTab ==` chỉ còn 8 chỗ (7 dispatch + 1 tab button).
4. **Build sạch**: `build_fast.bat` exit 0, **0 warning**; `build.bat` full 25 tool exit 0, 0 error.
5. **Aim math**: `ext_overlay.exe -aimtest` exit 0, không FAIL (trước và sau full build).
6. **Menu**: `backup\s1_hits_final_artifact.txt` khớp `backup\s1_baseline_hits.txt` 8/8 cả hits và size.
7. **Tooling**: build từ System32 OK; fail-injection trả exit 1.

---

## 6. Sai lệch so với `PLAN_S1_split_menu_tabs.md`

1. **C3 gộp vào C1** (include dwmapi bị xoá cùng batch C1). Không ảnh hưởng; `-ldwmapi` vẫn giữ.
2. **`g_diagState` bỏ static sớm ở S1.2** (vì `keyBindWidget` move sang `ovl_ui.cpp` cần extern). Do đó script Appendix B sinh **85** extern, không phải 86; `g_diagState` và `g_menuHits` được xử lý tay.
3. **Extern bổ sung ngoài danh sách 87** (phát hiện lúc compile từng tab — đúng fallback của plan):
   - `extern SrvState srv;`
   - `extern uint64_t wLighting, wTerrain, wAtmosphere;`
   - `extern uint64_t wBloom, wBlur, wCC, wLocalPlr;`
   - `extern const int kColorPal[11];` (bound tường minh để `sizeof` còn chạy)
   - `float velextAlphaFromDt(float, float);`
   - `void saveWaypoints();`
4. **`MenuCtx::ents` là non-const** (`std::vector<Ent>&`) thay vì `const&`: body Teleport dùng `Ent& e = ents[i]`.
5. **`memory.h` đổi cơ chế asm** (mục 4.4) — ngoài plan, bắt buộc để multi-TU link được.
6. **Build scripts sửa thêm 3 điểm tooling** (mục 4.5) ngoài danh sách nguồn.
7. `tools\s1_ids.txt` vẫn liệt kê 87 id (gồm `g_menuHits`) — nếu chạy lại generator Appendix B, nhớ bổ sung tay `g_diagState`/`srv`/`w*`/`velextAlphaFromDt`/`saveWaypoints`.

---

## 7. Việc còn lại (cần test tay trong game)

Gate tự động chỉ chứng minh: build/link, aim math, menu layout + hit-test, đọc offsets (`vm=1`, `tpdiag selfOk=1 rows=1`). **Chưa** chứng minh đường ghi memory và hành vi từng tính năng. Checklist:

- [ ] Body: Fly bật/tắt (F), Speed, Jump; revert về giá trị gốc khi tắt.
- [ ] Combat: Aim (hold/toggle), FOV slider, smoothing, trigger, autoclick.
- [ ] Visual: ESP box/name/hp/skel/oof, radar, đổi màu, fill.
- [ ] Teleport: player list, click TP, waypoint add/TP/save, place profile.
- [ ] World: clock freeze, brightness, fog, FOV, atmosphere, water/grass, nút reset.
- [ ] Server: info/ping, list server, join, hop, anti-AFK.
- [ ] Misc: profile save/load, panic key, collapse/expand, kéo menu + lưu vị trí.
- [ ] Kiểm tra không có instance thứ 2 (mutex) và không chạy song song `velext`.

Ghi chú: `HANDOFF_world_tab.md` §7 vẫn là checklist World chi tiết chưa xác nhận bằng mắt.

---

## 8. Rollback

| Mốc | File |
|---|---|
| Trước C1/S1 | `backup\overlay_S1PRE_20261020.cpp` (7.217 dòng), `backup\ext_overlay_S1PRE_20261020.exe`, `backup\build_fast_S1PRE.bat`, `backup\build_S1PRE.bat` |
| Sau C1 | `backup\s1_hits_after_C1.txt`, `backup\C1_removed_lines.txt` |
| Sau C2 | `backup\s1_hits_after_C2.txt` |
| Sau S1 (snapshot) | `backup\S1POST_20261020\` (overlay.cpp, offsets.*, memory.h, 2 build script, `ovl\*`, `sha256.txt`) |

Khôi phục nhanh về trước S1:
```powershell
Copy-Item backup\overlay_S1PRE_20261020.cpp src\overlay.cpp
Copy-Item backup\build_fast_S1PRE.bat   build_fast.bat
Copy-Item backup\build_S1PRE.bat        build.bat
Remove-Item src\ovl -Recurse
```
(C2 không có backup riêng; field bị xoá liệt kê đầy đủ trong `DEAD_CODE_AUDIT.md` §G4. Muốn revert `memory.h`, dùng block gốc ở Appendix A.)

---

## 9. Gợi ý S2/S3

`overlay.cpp` còn **6.123 dòng**, các khối lớn còn lại:
- cfg parser + IO (`loadCfg`/`saveCfg`/`upsertCfgKey`/`exePath`/profile/waypoints) — tách được sớm, ít rủi ro.
- server worker + watchdog (~700 dòng) — đã có `HANDOFF_server_tab.md`.
- worldTick (scan Lighting/Atmosphere/Terrain) — tách theo cùng pattern.
- entity cache 3 tầng + ESP render + aim math — rủi ro cao nhất (14 lambda `[&]`, state chia sẻ trong `main`).

---

## Appendix A — `memory.h` block gốc (trước S1, để revert)

```cpp
extern "C" {
// NtWriteVirtualMemory syscall id, resolved at runtime from the ntdll stub.
__attribute__((used)) static uint32_t g_ntWriteId = 0;
long ntWriteSyscall(void* handle, void* addr, const void* buf, size_t size,
                    size_t* written);
}

// mov eax, [g_ntWriteId] ; mov r10, rcx ; syscall ; ret
// (rdx/r8/r9 + the 5th arg at [rsp+0x28] are already where the kernel wants them)
__asm__(
    ".text\n"
    ".globl ntWriteSyscall\n"
    ".def ntWriteSyscall; .scl 2; .type 32; .endef\n"
    "ntWriteSyscall:\n"
    "    movl g_ntWriteId(%rip), %eax\n"
    "    movq %rcx, %r10\n"
    "    syscall\n"
    "    ret\n");
```

## Appendix B — lệnh verify nhanh

```powershell
# build overlay (~30 s)
cmd /c build_fast.bat

# unit test aim
.\ext_overlay.exe -aimtest

# build toàn bộ tool (~3 phút)
cmd /c build.bat

# menu layout 1 tab (sandbox)
$d = "$env:TEMP\opencode\s1hits\tab4"
# copy ext_overlay.exe + offsets_cache.json + overlay.cfg (menu.tab=4, menu.show=1, menu.collapsed=0)
Push-Location $d; .\ext_overlay.exe -diag -nohide -t 4; Pop-Location
```

> Lưu ý harness: output `-diag` dùng CRLF. Nếu tách dòng bằng ``"`n"`` thì phải `TrimEnd([char]13)`,
> hoặc dùng `-split "`r?`n"`. Quên bước này + `Tee-Object` sẽ sinh dòng rỗng và làm lệch so sánh
> (đã gặp ở lần đo cuối; số liệu exe vẫn đúng, chỉ file evidence bị chèn dòng trống).
