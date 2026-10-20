# PLAN S2 — Tách cfg IO, server module, world module khỏi `src\overlay.cpp`

> **Snapshot đo lúc:** 2026-10-20 (sau S1, trước mọi thay đổi S2).
> `src\overlay.cpp` = **6.123 dòng / 280.220 B**; `ext_overlay.exe` hiện tại là bản S1.
> Mọi số dòng dưới đây là của snapshot này. **Khi implement luôn tìm bằng marker**
> (`// ---- exe-relative`, `bool upsertCfgKey`, `static void loadAimCfg`, `// ---- profiles`,
> `// ---- waypoints`, `// ---- World (Đợt 2)`, `// ====...Server tab`, `runSrvSelfTest`),
> không bám số dòng cứng.

---

## 0. TL;DR

Tách 3 khối còn lại có ranh giới rõ, **không đụng aim/ESP/entity cache**:

| Khối | Dòng hiện tại | Số dòng | Đích |
|---|---|---:|---|
| cfg IO (exe dir, toggle key, upsert, parser, profiles, waypoints) | 52–65, 67–175, 484–515, 740–1161 | **577** | `src\ovl\ovl_cfg.cpp` |
| Server module (JSON, HTTP worker, join, ping, watchdog, anti-AFK, self-test) | 1961–2943 | **983** | `src\ovl\ovl_server.cpp` |
| World module (Lighting/Atmosphere/Terrain/effects/FOV) | 1483–1959 | **477** | `src\ovl\ovl_world.cpp` |
| **Tổng move** | | **2.037** | |

Dự kiến `overlay.cpp` còn **~4.090 dòng** (−33%). Thêm 1 bước hạ tầng nhỏ
(`src\ntwrite.cpp`) để world module vẫn ghi memory bằng syscall như cũ, không âm thầm
rơi về `WriteProcessMemory`.

**Entity cache (3 tầng, ~306 dòng lambda trong `main`) KHÔNG làm ở S2** — xem §9.

---

## 1. Mục tiêu / phạm vi

### 1.1 Mục tiêu
1. `overlay.cpp` chỉ còn: khung menu, main loop, input/hotkey, aim/ESP, entity cache,
   player mods/teleport/fly, hitbox, diag, writer thread.
2. Sửa cfg/server/world chỉ cần đọc 1 file module + header tương ứng.
3. **Không đổi hành vi**: cùng cfg key, cùng default, cùng thứ tự tab, cùng menu,
   cùng aim/ESP, cùng thứ tự ghi memory.

### 1.2 Trong phạm vi
- `initExeDir`, `exePath`, `g_exeDir`, `loadToggleKey`.
- `upsertCfgKey`, `cfgInt`, `cfgFloat`, `loadAimCfg` (parser duy nhất; **không có `saveCfg`**
  trong code — mọi save là `upsertCfgKey` từng key).
- Profile: `copyFileRaw`, `profileFile`, `profileExists`, `saveProfileSlot`,
  `loadProfileSlot`, `cfgStrValue`, `setPlaceProfile`.
- Waypoint: `saveWaypoints`, `loadWaypoints`.
- Toàn bộ server block: JSON helpers, `SrvState`/`SrvWire` definitions, clipboard,
  `srvValidJob`, `srvShort`, `srvIpOnly`, `srvParseJoin`, `srvBuildUri`, `srvBuildLink`,
  `srvOpenJoin`, `srvReadPing`, `srvFilterRows`, `srvRequestList`, `srvRequestGeo`,
  `srvFetchList`, `srvFetchGeo`, `srvWorkerThread`, `srvDiffRoster`, `srvDrainWire`,
  `srvTick`, `runSrvSelfTest`.
- Toàn bộ world block: `WorldF/B/V3/State`, `worldSt`, `fovFmt`, `wf*`, `wb*`, `wv*`,
  `worldRestoreAll`, `worldResolve`, `worldTick`, `wLighting..wLocalPlr`.

### 1.3 Ngoài phạm vi (S3)
- `refreshEnts` + `classOf` + `resolveParts` + `clearResolvedParts` +
  `invalidateAimSnapshot` (~306 dòng, 14 lambda `[&]`, chia sẻ `g_snap*`, `stickPlayer`,
  `ecache`, `cclassCache` với aim/ESP/hitbox/world).
- Aim math, writer thread, ESP render, radar, teleport/fly, hitbox.
- Không đổi tên/không thêm/bớt cfg key, không đổi default, không đổi thứ tự tab.

### 1.4 Nguyên tắc
- **Move verbatim**: cắt nguyên khối, không format lại, không sửa logic.
- Chỉ bỏ `static` khi symbol cần export; thêm `extern` đúng type.
- Mỗi bước có build + verify riêng, không gộp.
- Build danh sách file **tường minh** trong cả `build_fast.bat` và `build.bat`.
- `src\overlay.cpp` và `src\ovl\*`: **LF-only, không BOM**.

---

## 2. Hiện trạng đo được (marker + số dòng)

| # | Khối | Bắt đầu (marker) | Kết thúc (marker) | Dòng | Ghi chú |
|---|---|---|---|---:|---|
| A | exe dir | `// ---- exe-relative side files` (52) | `exePath` `}` (65) | 14 | `g_exeDir`, `initExeDir`, `exePath` |
| B | toggle key | `static int loadToggleKey()` (67) | `}` (175) | 109 | template cfg ~90 dòng |
| C | upsert | `bool upsertCfgKey` (484) | `}` (515) | 32 | ghi từng key, giữ thứ tự |
| D | parser | `static int cfgInt` (740) | `}` loadAimCfg (1048) | 309 | 110 biến cfg/g_* |
| E | profiles | `// ---- profiles` (1050) | `}` setPlaceProfile (1117) | 68 | byte-copy profile |
| F | waypoints | `// ---- waypoints` (1119) | `}` loadWaypoints (1161) | 43 | `wp.<n>` |
| G | world | `// ---- World (Đợt 2)` (1483) | `}` worldTick (1959) | 477 | có memory write |
| H | server | `// ====...Server tab` (1961) | `}` runSrvSelfTest (2943) | 983 | worker thread + watchdog |

Kiểm chứng ranh giới:
- 1960 trống; 1961 banner server; 2943 `}`; 2944 trống; 2945 `buildAimMatrix` (aim — giữ).
- 1482 trống; 1483 comment world; 1959 `}`; 1960 trống.
- 739 trống; 740 `cfgInt`; 1161 `}`; 1162 trống; 1163 `// ---- hitbox` (giữ).
- 66 trống; 67 `loadToggleKey`; 175 `}`; 176 trống; 177 `QUIT_VK` (giữ).
- 516–549 `keyCfgName` (giữ — chỉ main dùng); 550–739 khai báo cfg (giữ định nghĩa).

### 2.1 Vì sao thứ tự S2.1 → cfg → server → world

`loadAimCfg` gọi `srvValidJob` (server). Nếu làm cfg trước, chỉ cần de-static
`srvValidJob` + khai báo trong `ovl_server.h`; definition tạm ở `overlay.cpp` đến khi
server move. Không cần đảo thứ tự, không duplicate logic.

World làm sau cùng vì có memory write; làm sau `ntwrite.cpp` để giữ đúng đường syscall.

---

## 3. Kiến trúc đích

### 3.1 Cây file sau S2

```
src\
  ntwrite.cpp              ~22   định nghĩa DUY NHẤT g_ntWriteId + ntWriteSyscall
  memory.h                 sửa   extern thay vì static per-TU
  overlay.cpp             ~4.090 khung menu, main loop, aim/ESP, entity cache,
                                 player mods, teleport/fly, hitbox, diag, writer
  ovl\
    ovl_types.h            (không đổi)
    ovl_ui.h/.cpp          (không đổi logic; bỏ prototype upsertCfgKey)
    ovl_state.h            (không đổi)
    ovl_tabs.h             (không đổi)
    ovl_core.h             ~12   diagRate (cross-module)
    ovl_cfg.h              ~40   API cfg + 43 extern
    ovl_cfg.cpp            ~600  khối A–F verbatim
    ovl_server.h           ~25   API server + g_srvRun
    ovl_server.cpp         ~1.000 khối H verbatim
    ovl_world.h            ~18   API world
    ovl_world.cpp          ~490  khối G verbatim
    tab_*.cpp              (không đổi)
```

### 3.2 Ai sở hữu gì

| File | Sở hữu | Ai đọc khi sửa |
|---|---|---|
| `ovl_cfg.cpp` | đọc/ghi `overlay.cfg`, profile, waypoint | task cfg/profile/wp |
| `ovl_server.cpp` | server list, join, ping, watchdog, anti-AFK | task Server tab |
| `ovl_world.cpp` | Lighting/Atmosphere/Terrain/FOV | task World tab |
| `overlay.cpp` | menu, loop, aim/ESP, entity cache, mods | task hành vi chung |

### 3.3 Include graph (không tạo vòng)

```
overlay.cpp     -> ovl_types, ovl_ui, ovl_state, ovl_tabs,
                   ovl_core, ovl_cfg, ovl_server, ovl_world
ovl_cfg.cpp     -> ovl_cfg, ovl_state, ovl_ui (parseKey), ovl_server (srvValidJob)
ovl_server.cpp  -> ovl_server, ovl_state, ovl_core, ovl_ui (toast), ovl_cfg (upsert),
                   offsets.h (httpGetEx), sdk.h
ovl_world.cpp   -> ovl_world, ovl_state, ovl_core, sdk.h
ovl_server.h    -> sdk.h, ovl_types.h, <atomic>, <string>, <vector>
ovl_world.h     -> sdk.h, <cstdint>
ovl_cfg.h       -> ovl_types.h (DWORD/Vec3), <string>, <cstdint>
ovl_core.h      -> <windows.h>
```

Không cần thêm `-I`: `-Isrc` đã có; trong `src\ovl\` include sibling trực tiếp.

---

## 4. Hợp đồng dữ liệu

### 4.1 `ovl_core.h`

```cpp
#pragma once
#include <windows.h>
// diagRate: định nghĩa ở overlay.cpp (g_diagFile + mutex), dùng bởi server/world.
void diagRate(const char* key, DWORD intervalMs, const char* fmt, ...);
```

### 4.2 `ovl_cfg.h`

```cpp
#pragma once
#include "ovl_types.h"
#include <cstdint>
#include <string>

extern std::string g_exeDir;

void initExeDir();
std::string exePath(const char* name);
int  loadToggleKey();
void loadAimCfg();
bool upsertCfgKey(const char* key, const char* value);
std::string cfgStrValue(const std::string& key);
bool profileExists(const std::string& slot);
bool saveProfileSlot(const std::string& slot);
bool loadProfileSlot(const std::string& slot);
void setPlaceProfile(uint64_t place, const std::string& slot);
void loadWaypoints();
// saveWaypoints: giữ prototype trong ovl_state.h (tab Teleport đã dùng).

// 43 biến đang static, parser cần đọc -> bỏ static tại định nghĩa, extern ở đây:
extern float g_menuW, g_menuH, g_savedMenuW, g_savedMenuH;
extern int cfgACKeyMode, cfgACInterval, cfgACDuration, cfgACPress;
extern int cfgAimKeyMode, cfgAimSmooth, cfgAimMode, cfgAimSticky;
extern float cfgAimStickTime;
extern int cfgAimPredict;
extern float cfgAimPScale, cfgAimDynK;
extern int cfgAimNearest;
extern float cfgAimOverextend, cfgAimHeight, cfgAimMaxDist, cfgAimDeadzone;
extern int cfgAimIgnoreTeam, cfgAimInterval, cfgAimBone, cfgAimNPCs;
extern float cfgAimTSmooth, cfgAimSens, cfgAimGameSens;
extern int cfgAimPid;
extern float cfgAimMemSmooth;
extern int cfgAimRate;
extern int cfgTrigDelay, cfgTrigCooldown, cfgTrigPress, cfgTrigPad;
extern int cfgChatGuard;
extern int cfgPlySdnKey, cfgPlySupKey, cfgPlyJdnKey, cfgPlyJupKey;
extern int cfgMenuCollapsed;
extern std::string cfgSrvLastJob;
extern uint64_t cfgSrvLastPlace;
```

> `cfgSrvLastJob`/`cfgSrvLastPlace` **move định nghĩa** sang `ovl_cfg.cpp` (không còn
> consumer nào trong `overlay.cpp` sau khi server move). Các biến còn lại vẫn định nghĩa
> ở `overlay.cpp`, chỉ bỏ `static`.

### 4.3 `ovl_server.h`

```cpp
#pragma once
#include "sdk.h"          // Memory, SDK, Offsets
#include "ovl_types.h"    // SrvState, SrvWire
#include <atomic>
#include <string>
#include <vector>

extern std::atomic<bool> g_srvRun;   // main set false lúc thoát, worker đọc

void srvWorkerThread();
void srvTick(Memory& m, SDK& sdk, const Offsets& o, DWORD now,
             bool fgGame, bool typingNow, bool aiming,
             const std::vector<std::string>& names, int entCount);
int  runSrvSelfTest(uint64_t place);
bool srvValidJob(const std::string& s);
```

Các prototype tab đang dùng (`srvShort`, `srvFilterRows`, `srvRequestList`,
`srvOpenJoin`, `srvClipGet/Set`, `srvParseJoin`, `srvBuildLink`) **giữ nguyên**
trong `ovl_state.h`; definition chuyển sang `ovl_server.cpp`.

### 4.4 `ovl_world.h`

```cpp
#pragma once
#include "sdk.h"
#include <cstdint>

void worldRestoreAll(Memory& m, const Offsets& o);
void worldTick(Memory& m, SDK& sdk, const Offsets& o, bool haveWrite,
               DWORD now, uint64_t camAddr);
```

`wLighting/wTerrain/wAtmosphere/wBloom/wBlur/wCC/wLocalPlr` đã có extern trong
`ovl_state.h`; **định nghĩa move** sang `ovl_world.cpp`.

### 4.5 `memory.h` + `src\ntwrite.cpp` (S2.1)

`memory.h` đổi:

```cpp
extern "C" {
extern uint32_t g_ntWriteId;
long ntWriteSyscall(void* handle, void* addr, const void* buf, size_t size,
                    size_t* written);
}
```

`src\ntwrite.cpp`:

```cpp
#include <cstddef>
#include <cstdint>

extern "C" {
__attribute__((used)) uint32_t g_ntWriteId = 0;

__attribute__((naked)) long ntWriteSyscall(void* handle, void* addr,
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

Lý do: sau khi tách world, write từ `ovl_world.cpp` sẽ có `g_ntWriteId = 0` (static per-TU)
và **âm thầm fallback WPM**. Shared TU giữ nguyên đường syscall cho mọi TU, đúng hành vi
trước S2 (một `Memory` instance, một id).

**Build lines cần thêm `src\ntwrite.cpp`:** overlay (2 script) + `ext_ntcheck.exe` +
`ext_wpmtest.exe` (2 tool duy nhất trong `build.bat` có `openWrite`/`.write`).
4 tool còn lại (`clockhold/fovpair/hbcheck/worldprobe`) không nằm trong build script.

---

## 5. Các bước triển khai

### S2.0 — Snapshot + baseline (10 phút)

1. Tạo `backup\S2PRE_20261020\`:
   - `overlay.cpp`, `memory.h`, `ovl\` (13 file), `offsets.h/cpp`, `build_fast.bat`, `build.bat`
   - `ext_overlay_S2PRE.exe` (bản S1 đang chạy) — dùng làm reference cho default-cfg test
   - `overlay.cfg` + `sha256.txt`
2. `build_fast.bat` → exit 0, đếm warning (kỳ vọng 0).
3. `ext_overlay.exe -aimtest` → exit 0, 0 `FAIL`.
4. `ext_overlay.exe -srvtest 0` → `=== done (PASS) ===` (network chỉ WARN).
5. Đo hits 8 tab → `backup\s2_baseline_hits.txt`; diff với `backup\s1_baseline_hits.txt`.

**Gate S2.0:** build 0/0; aimtest 0; srvtest PASS; hits 8/8 khớp baseline S1
(`hits` + `size`); `sha256(overlay.cfg)` không đổi sau các lần chạy.

**Rollback:** không cần (chưa sửa).

---

### S2.1 — Shared NT write TU (20 phút)

1. Tạo `src\ntwrite.cpp` (§4.5).
2. Sửa `src\memory.h`: bỏ `static`/definition, để `extern` declaration + comment mới.
3. Thêm `src\ntwrite.cpp` vào:
   - `build_fast.bat` (dòng overlay)
   - `build.bat` dòng overlay, `ext_ntcheck`, `ext_wpmtest`.
4. Build + chạy `ext_ntcheck.exe` (không `--write`): kỳ vọng
   `resolved NtWriteVirtualMemory id = 58 (0x3a) directWrite=1` và
   `MATCH direct-call ABI OK`.

**Gate S2.1:** `build_fast` exit 0, 0 warning; `-aimtest` 0; hits 8/8;
`build.bat` full 0 error; `ext_ntcheck` id=58 + MATCH.

**Rủi ro:** linker `undefined reference` ở tool quên thêm ntwrite → build.bat bắt ngay.
**Rollback:** copy `memory.h` từ S2PRE, xoá `ntwrite.cpp`, revert 4 dòng build.

---

### S2.2 — `ovl_cfg.cpp` (45 phút)

1. Tạo `ovl_cfg.h` (§4.2) + `ovl_cfg.cpp`.
2. Move verbatim 4 khối: A (52–65), B (67–175), C (484–515), D+E+F (740–1161)
   sang `ovl_cfg.cpp`, theo thứ tự: exe dir → toggle key → upsert → parser → profiles →
   waypoints (giữ nguyên comment + blank).
3. `overlay.cpp`:
   - thêm `#include "ovl/ovl_cfg.h"`, `#include "ovl/ovl_core.h"`,
     `#include "ovl/ovl_server.h"` (cho `srvValidJob`);
   - xoá 4 khối đã move;
   - xoá forward decl `static bool srvValidJob(...)` (dòng 738) — thay bằng prototype
     trong `ovl_server.h`;
   - bỏ `static` ở 43 định nghĩa (§4.2), **giữ nguyên giá trị khởi tạo**;
   - `srvValidJob` definition: bỏ `static` (tạm ở `overlay.cpp` đến S2.3).
4. `ovl_ui.h`: xoá prototype `upsertCfgKey` + comment cũ; `ovl_ui.cpp` thêm
   `#include "ovl_cfg.h"`.
5. `g_exeDir` + `cfgSrvLastJob/Place` định nghĩa move sang `ovl_cfg.cpp`.

**Gate S2.2:**
- build 0 lỗi/0 warning; `-aimtest` 0.
- **Parser key-set diff:** script trích mọi literal key trong `loadAimCfg`
  (`strncmp(line,"KEY="...)`, `cfgInt/cfgFloat(line,"KEY"...)`) từ
  `backup\S2PRE_20261020\overlay.cpp` và từ `ovl_cfg.cpp` → **set giống hệt**, không mất/
  không thêm key.
- **Move verbatim:** so text 4 khối (bỏ `static ` đầu dòng khi cần) với backup →
  identical ngoài các dòng de-static.
- **Round-trip cfg thật:** sandbox copy `overlay.cfg`; chạy `-diag -nohide -t 4` tab 0.
  Trước/sau phải: cùng số dòng, cùng thứ tự key, **chỉ `menu.size` đổi value**
  (app tự ghi khi size ổn định 20 frame); không mất/không nhân key.
- **Default template:** sandbox không có `overlay.cfg`; chạy bản S2PRE và bản mới
  (tuần tự, không song song) `-diag -nohide -t 3` → 2 file `overlay.cfg` sinh ra
  **byte-identical** (cùng template + cùng `menu.size` write).
- **Sentinel parse:** cfg fixture `toggle=H, aim.key=Q (81), aim.keymode=0, aim.mode=2,
  aim.rate=777, aim.smoothing=13, aim.pid=1, menu.tab=7, menu.pos=200,150` → startup diag
  phải in `mode=2 key=81 keymode=0 rate=777 smoothing=13.00 pid=1`; menu line `tab=7`,
  `hits>0`; toggle name `H`.
- hits 8/8 khớp baseline.

**Rollback:** restore `overlay.cpp`, `ovl_ui.h/.cpp` từ S2PRE; xoá `ovl_cfg.*`; revert
include ở `overlay.cpp` (không đụng build vì chưa thêm file mới vào build ở bước này?
→ **có**: thêm `src\ovl\ovl_cfg.cpp` vào cả 2 script ngay khi tạo file).

---

### S2.3 — `ovl_server.cpp` (40 phút)

1. Tạo `ovl_server.h` (§4.3) + `ovl_server.cpp`.
2. Move verbatim toàn bộ 1961–2943 (banner → `runSrvSelfTest`).
3. Move definitions: `SrvState srv;`, `SrvWire g_srvWire;`, `int g_srvSel = -1;`,
   `std::atomic<bool> g_srvRun{true};` + toàn bộ static worker state.
4. Move `srvValidJob` definition từ `overlay.cpp` sang `ovl_server.cpp`; bỏ `static`.
5. `overlay.cpp`: xoá khối; thêm include `ovl_server.h`; `diagRate` bỏ `static`;
   main giữ nguyên `std::thread srvTh(srvWorkerThread);`, `srvTick(...)`,
   `runSrvSelfTest(...)`, `g_srvRun.store(false)`.
6. Thêm `src\ovl\ovl_server.cpp` vào **cả 2** build script.

**Gate S2.3:**
- build 0/0; aimtest 0.
- `-srvtest 0` → `=== done (PASS) ===` (paser/join/filter/clipboard; network WARN).
- hits 8/8 (tab 5 đặc biệt: `hits=23`, `size=344,455`).
- Thread: chạy `-diag -nohide -t 6` tab 5, thoát sạch (`overlay exit OK`, không treo);
  kiểm không có `ext_overlay` sót; `srvWorkerThread` join qua `g_srvRun=false`.
- Diag tab 5 vẫn in info/list (nếu offline: listState=3 + toast fail, không crash).
- **Không deadlock:** worker chỉ giữ `g_srvWire.mu` khi publish; `srvRequestList` giữ
  cùng mutex rất ngắn; `srvDrainWire` copy rồi mới xử lý. Move nguyên văn ⇒ giữ nguyên
  thứ tự lock.

**Rollback:** restore `overlay.cpp` từ S2PRE (hoặc từ snapshot S2.2), xoá `ovl_server.*`,
revert 2 build script.

---

### S2.4 — `ovl_world.cpp` (30 phút)

1. Tạo `ovl_world.h` (§4.4) + `ovl_world.cpp`.
2. Move verbatim 1483–1959, gồm definitions `wLighting..wLocalPlr` và `worldSt`.
3. `overlay.cpp`: xoá khối; thêm include `ovl_world.h`; `worldRestoreAll`/`worldTick`
   bỏ `static`.
4. Thêm `src\ovl\ovl_world.cpp` vào **cả 2** build script.

**Gate S2.4:**
- build 0/0; aimtest 0; hits 8/8 (tab 4: `hits=25`, `size=344,455`).
- `ext_ntcheck` vẫn MATCH (chứng minh shared syscall TU).
- Trong game: bật/tắt từng mục World, quan sát restore (checklist §8).
- Không còn `wLighting`/`worldSt`/`worldTick` body trong `overlay.cpp`
  (chỉ còn call sites + extern).

**Rollback:** restore `overlay.cpp`, xoá `ovl_world.*`, revert 2 build script.

---

### S2.5 — Full verify + docs + snapshot (45 phút)

1. `build_fast.bat` → exit 0, **0 warning** (ghi log).
2. `build.bat` full → 25 tool, 0 error.
3. `-aimtest` 0; `-srvtest 0` PASS.
4. Hits 8/8 → `backup\s2_hits_final.txt`; diff `hits`+`size` với
   `backup\s1_baseline_hits.txt`.
5. Cfg re-check: parser key-set, default template, round-trip, sentinel.
6. `objdump -h` so `.text` trước/sau (chỉ để biết, không làm gate).
7. Viết `HANDOFF_S2_cfg_server_world.md`:
   - file map + line counts trước/sau;
   - thay đổi ngoài move (de-static, include, ntwrite, build);
   - bằng chứng verify (log + số đo);
   - sai lệch so với plan;
   - checklist in-game;
   - rollback.
8. Snapshot `backup\S2POST_20261020\` + `sha256.txt`.
9. Cập nhật `AGENTS.md`: cấu trúc mới, luật "cfg/server/world sửa file nào",
   luật `ntwrite.cpp` (mọi tool mới có `openWrite` phải link file này).

**Gate cuối:** §10.

---

## 6. Build scripts — thay đổi chính xác

### `build_fast.bat` (1 dòng overlay)

Thêm `src\ntwrite.cpp` + 3 file module vào danh sách hiện tại:

```bat
"%GCC%" -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp src\ntwrite.cpp src\ovl\ovl_ui.cpp src\ovl\ovl_cfg.cpp src\ovl\ovl_server.cpp src\ovl\ovl_world.cpp src\ovl\tab_body.cpp src\ovl\tab_combat.cpp src\ovl\tab_visual.cpp src\ovl\tab_teleport.cpp src\ovl\tab_world.cpp src\ovl\tab_server.cpp src\ovl\tab_tools.cpp src\ovl\tab_misc.cpp lib\libimgui.a -I%IMGUI% -I%IMGUI%\backends -Isrc -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
```

### `build.bat`

- Dòng overlay (26): **cùng danh sách y hệt** `build_fast.bat`.
- `ext_wpmtest.exe` (dòng 40) và `ext_ntcheck.exe` (dòng 59): thêm `src\ntwrite.cpp`.
- Không đổi tool khác.

Kiểm tra: `diff` 2 dòng overlay phải giống nhau 100%.

---

## 7. Verification protocol

### 7.1 Build
- `build_fast.bat` exit 0, 0 warning; `build.bat` full 0 error.
- Chạy `build_fast.bat` từ `C:\Windows\System32` (test CWD) — 1 lần ở S2.5.

### 7.2 Aim math + server self-test
- `ext_overlay.exe -aimtest` → exit 0, không dòng `FAIL`.
- `ext_overlay.exe -srvtest 0` → exit 0, `=== done (PASS) ===`.

### 7.3 Menu hit-test 8 tab
- Script sandbox `%TEMP%\opencode\s2hits\tabN`, copy exe + offsets_cache + cfg;
  sửa `menu.tab=N`, `menu.show=1`, `menu.collapsed=0`.
- `ext_overlay.exe -diag -nohide -t 4`; tách dòng `-split "`r?`n"` (CRLF).
- So `hits=` + `size=` với `backup\s1_baseline_hits.txt`; `pos=` phải `928,173`.

### 7.4 Cfg
- **Key-set:** script so literal key set của parser trước/sau.
- **Default template:** sandbox trống, chạy S2PRE exe và exe mới, so file sinh ra.
- **Round-trip:** copy cfg thật, chạy 4 s, diff line-by-line: chỉ `menu.size` đổi,
  cùng thứ tự key, không mất key.
- **Sentinel:** fixture §S2.2, so startup diag + menu diag.

### 7.5 Server thread
- Tab 5 chạy 6 s, thoát sạch; không treo ở `srvTh.join()`.
- Kiểm `Get-Process ext_overlay` rỗng sau mỗi lần chạy.
- Offline: list fail hiển thị toast, không crash.

### 7.6 Memory write
- `ext_ntcheck.exe`: id 58, ABI MATCH.
- Sau S2.4: lặp lại (shared TU vẫn đúng).

---

## 8. Checklist in-game (user làm sau gate tự động)

- [ ] Body: Fly/Speed/Jump bật/tắt, revert đúng.
- [ ] Combat: Aim hold/toggle, FOV, smoothing, trigger, autoclick.
- [ ] Visual: ESP box/name/hp/skel/oof, radar, màu, fill.
- [ ] Teleport: list player, TP, waypoint add/TP/save, profile place.
- [ ] World: fullbright, brightness, fog, shadows, expo, clock, fx, atmo,
      water, grass, FOV, zoom; nút reset; thoát overlay restore hết.
- [ ] Server: info/ping, list server, join, hop, anti-AFK, auto-rejoin.
- [ ] Misc: profile save/load p1..p9, panic, collapse, kéo menu + lưu pos.
- [ ] Bind phím (Misc) → ghi đúng key vào cfg; slider → ghi đúng value.
- [ ] Không chạy song song `velext`; không có instance thứ 2.

---

## 9. Vì sao KHÔNG tách entity cache ở S2

`refreshEnts` + 3 helper là **lambda trong `main`** (3628–3925), capture `[&]`:
`ents, sdk, m, o, classCache, ecache, cDm, cPlayers, lastPlaceSeen, uisAddr,
npcScanSkip, stickPlayer` + gọi `loadHitboxProfile`, `hitboxRestoreAll`,
`worldRestoreAll`, `invalidateAimSnapshot` (đụng `g_snapMu/g_snap/g_snapEpoch`).

Tách an toàn cần một `EntityCtx` sống lâu, trong đó có reference tới state aim và
hitbox/world; đó chính là vùng S3 (aim/ESP). Làm bây giờ vừa rủi ro cao vừa vi phạm
"This S2 chưa đụng aim/ESP". Để S3 với gate riêng.

---

## 10. Definition of Done

1. `src\ntwrite.cpp`, `ovl_core.h`, `ovl_cfg.h/.cpp`, `ovl_server.h/.cpp`,
   `ovl_world.h/.cpp` tồn tại; build scripts liệt kê đủ.
2. `overlay.cpp` **≤ 4.200 dòng** (dự kiến ~4.090); không còn body của 3 khối.
3. `grep` trong `overlay.cpp`: không còn `static void loadAimCfg`, `srvWorkerThread`,
   `worldTick`, `worldRestoreAll`, `upsertCfgKey` body.
4. `build_fast` 0 warning; `build.bat` 0 error; chạy được từ CWD khác.
5. `-aimtest` 0 FAIL; `-srvtest 0` PASS.
6. Hits 8 tab khớp baseline S1 **8/8** (`hits` + `size`).
7. Cfg: key-set identical; default template byte-identical; round-trip chỉ đổi
   `menu.size`; sentinel parse đúng.
8. `ext_ntcheck` id=58 + MATCH.
9. `HANDOFF_S2_cfg_server_world.md` + `backup\S2POST_20261020\` + `sha256.txt`.
10. `AGENTS.md` cập nhật cấu trúc + luật ntwrite.

---

## 11. Ước lượng

| Bước | Thời gian |
|---|---|
| S2.0 snapshot + baseline | 10' |
| S2.1 ntwrite | 20' |
| S2.2 cfg | 45' |
| S2.3 server | 40' |
| S2.4 world | 30' |
| S2.5 verify + docs + snapshot | 45' |
| **Tổng** | **~3 giờ** |

Lợi ích đọc file:

| Task | Trước S2 | Sau S2 |
|---|---|---|
| Sửa cfg/parser/profile/wp | 6.123 dòng | ~600 + 40 header |
| Sửa server | 6.123 | ~1.000 + 25 |
| Sửa world | 6.123 | ~490 + 18 |
| Sửa menu/aim/ESP | 6.123 | ~4.090 |

---

## 12. Rủi ro & cách chặn

| Rủi ro | Xác suất | Chặn |
|---|---|---|
| Quên de-static 1 biến cfg | Cao | Compiler báo `'static' declaration follows non-static` |
| Thiếu extern → link error | Cao | Build từng bước; lỗi `undefined reference` chỉ đúng tên |
| Quên `ntwrite.cpp` ở tool | Trung bình | `build.bat` full; `ext_ntcheck`/`ext_wpmtest` sẽ fail link |
| Worker deadlock sau move | Thấp | Move verbatim; test tab 5 + exit sạch 6 s |
| World mất syscall write | Trung bình | S2.1 shared TU + `ext_ntcheck` MATCH |
| `menu.size` khác baseline | Thấp | Harness dùng đúng cfg gốc + `size=` đo thực tế, không phụ thuộc cfg size |
| cfg default template lệch | Thấp | So file sinh ra giữa S2PRE và bản mới |
| Chạy 2 overlay | Thấp | Kiểm `Get-Process ext_overlay` trước mỗi harness |
| Build script lệch nhau | Thấp | Diff 2 dòng overlay ở S2.5 |

---

## 13. Rollback

| Mốc | File |
|---|---|
| Trước S2 | `backup\S2PRE_20261020\` (overlay.cpp, memory.h, ovl\*, offsets.*, 2 build script, exe, cfg, sha256) |
| Sau S2.1 | `backup\S2.1_ntwrite\` (memory.h, ntwrite.cpp, 2 build script) |
| Sau S2.2 | `backup\S2.2_cfg\` (overlay.cpp, ovl_cfg.*, ovl_ui.*, 2 build script) |
| Sau S2.3 | `backup\S2.3_server\` (overlay.cpp, ovl_server.*, 2 build script) |
| Sau S2.4 | `backup\S2.4_world\` (overlay.cpp, ovl_world.*, 2 build script) |
| Sau S2 | `backup\S2POST_20261020\` + sha256 |

Khôi phục toàn bộ:

```powershell
Copy-Item backup\S2PRE_20261020\overlay.cpp src\overlay.cpp
Copy-Item backup\S2PRE_20261020\memory.h    src\memory.h
Copy-Item backup\S2PRE_20261020\build_fast.bat .
Copy-Item backup\S2PRE_20261020\build.bat .
Copy-Item backup\S2PRE_20261020\ovl\* src\ovl\ -Force
Remove-Item src\ntwrite.cpp,src\ovl\ovl_cfg.*,src\ovl\ovl_server.*,src\ovl\ovl_world.*,src\ovl\ovl_core.h -ErrorAction SilentlyContinue
```

Không đổi `overlay.cfg` schema ⇒ rollback không cần migrate dữ liệu.

---

## 14. Thứ tự thực thi (tóm tắt)

```
S2.0  snapshot + baseline        -> gate: 8/8 hits, build 0, aimtest 0
S2.1  ntwrite.cpp + memory.h     -> gate: ntcheck MATCH, full build 0
S2.2  ovl_cfg.cpp (577 dòng)     -> gate: cfg key-set/template/round-trip/sentinel
S2.3  ovl_server.cpp (983 dòng)  -> gate: srvtest PASS, tab5 6s sạch
S2.4  ovl_world.cpp (477 dòng)   -> gate: tab4, ntcheck lại
S2.5  full verify + docs + snap  -> gate: §10
```

**Chờ duyệt plan này trước khi sửa file nguồn đầu tiên.**
