# PLAN S1 — Tách 8 tab menu + helper UI khỏi `src\overlay.cpp`

> **Snapshot đo lúc:** 2026-10-20 01:25 (ngay sau build tab Server).
> `src\overlay.cpp` = **7.217 dòng / 331.433 B**; `ext_overlay.exe` = 7.775.980 B.
> Mọi số dòng trong tài liệu này là của snapshot đó. **Khi implement luôn tìm bằng marker**
> (`// ---- Clickable`, `cfgMenuTab == N`, `---------------- Misc`, `// ---- status footer`),
> không bám số dòng cứng — file đang tăng ~1.000 dòng/ngày.

---

## 0. TL;DR

Tách **thân 8 tab** (~802 dòng) + **helper UI dùng chung** (~125 dòng) + **vài struct** ra
`src\ovl\`. Giữ nguyên khung menu (kéo thả, tab buttons, footer, popup hit-test) trong
`overlay.cpp`. Không đổi hành vi, không đổi cfg key, không đụng aim/ESP/entity loop.

Kết quả: sửa 1 tab chỉ cần đọc `tab_<x>.cpp` (7–190 dòng) + 4 header (~300 dòng)
thay vì 7.217 dòng. `overlay.cpp` còn ~6.280 dòng.

---

## 1. Mục tiêu / phạm vi

### 1.1 Mục tiêu
1. Mỗi tab = 1 file `.cpp` riêng, tự chứa, để chat/AI build tab chỉ đọc file đó.
2. Giảm token + thời gian đọc file cho mọi task theo tab.
3. Không thay đổi hành vi: menu, aim, ESP, teleport, world, server, hit-test pixel.

### 1.2 Trong phạm vi (S1)
- 8 thân tab (Body, Combat, Visual, Teleport, World, Server, Tools, Misc).
- Helper UI dùng chung: `hitMark`, `chkInt`, `sliderFloatCfg`, `sliderIntCfg`,
  `keyName`, `keyStateLabel`, `keyBindWidget`, `markChildArea`, `toast`, `parseKey`.
- Struct dùng chung: `HitRect`, `Ent`, `Waypoint`, `SrvRow`, `SrvJoin`, `SrvState`, `SrvWire`.
- Khai báo `extern` cho 87 biến global + 12 hàm mà tab gọi.
- `MenuCtx` (bó biến main-local truyền vào tab).
- Cập nhật `build_fast.bat` + `build.bat` (danh sách file tường minh).

### 1.3 Ngoài phạm vi (để S2/S3)
- Config parser/saver (`loadCfg`, `upsertCfgKey` body, `saveCfg`).
- World module (`worldTick`, `worldRestoreAll`, 30 offset World).
- Hitbox module, server worker (`srvTick`, HTTP), profile IO, entity loop, aim/ESP.
- Không đổi tên cfg key, không đổi layout menu, không đổi thứ tự tab.

### 1.4 Nguyên tắc bắt buộc
- **Move verbatim**: cắt nguyên khối, không format lại, không "tiện tay" sửa logic.
- Sửa từng tab một, build + verify sau mỗi tab (không làm 8 tab rồi build 1 lần).
- Build dùng **danh sách file tường minh** — không glob `src\*.cpp` (trong `src\` có
  ~25 tool standalone có `main()` riêng).
- Không chạy song song 2 instance overlay (mutex `ext_overlay_singleton`).

---

## 2. Hiện trạng đo được

### 2.1 Bản đồ khối menu (snapshot 2026-10-20 01:25)

| Khối | Dòng | Ghi chú |
|---|---|---|
| `// ---- Clickable + draggable menu panel` | 5132 | bắt đầu khung menu |
| `g_menuHits.clear(); mouseLockUpdate(...)` | 5138–5139 | reset hit list mỗi frame |
| `ImGui::Begin("ext menu", ...)` | 5177 | cửa sổ menu |
| Tab buttons (2 hàng × 4) | 5257–5283 | **giữ trong overlay.cpp** |
| `ImGui::Separator();` | 5284 | |
| **Dispatch 8 tab** | **5286–6086** | **phần tách ra** |
| `// ---- status footer` | 6088–6097 | **giữ** (đọc `g_toast`, `tpMsg`, `keyName`) |
| Diag `menu tab=... hits=...` | 6099–6126 | **giữ** |
| `ImGui::End()` + popup hit-test | 6127–6154 | **giữ** |
| `// ---- ESP: cached list...` | 6156 | khối ESP (không đụng) |

### 2.2 Bảng tab + dependency chính xác

| # | Tab | Dòng | Số dòng | Biến main-local cần | Hàm ngoài cần |
|---|---|---|---|---|---|
| 0 | Body | 5286–5383 | 98 | `m`, `o`, `ents`, `flyApply`, `speedApply`, `jumpApply` | — |
| 1 | Combat | 5384–5437 | 54 | `haveWrite`, `aimOn`, `trigOn`, `acOn` | — |
| 2 | Visual | 5438–5529 | 92 | — (chỉ `g_colHover`) | — |
| 3 | Teleport | 5530–5719 | 190 | `m`, `o`, `ents`, `nowLoop`, `haveWrite` | `toast` |
| 4 | World | 5720–5812 | 93 | — | — |
| 5 | Server | 5813–5977 | 165 | `place`, `nowLoop` | `srvShort`, `srvFilterRows`, `srvRequestList`, `srvOpenJoin`, `srvClipGet`, `srvClipSet`, `srvParseJoin`, `srvBuildLink`, `toast` |
| 6 | Tools | 5978–5984 | 7 | — | — |
| 7 | Misc | 5985–6087 | 103 | `place`, `o`, `dbgEnts`, `haveWrite`, `loopMs` | `toast`, `profileExists`, `saveProfileSlot`, `loadProfileSlot`, `setPlaceProfile`, `keyBindWidget` |

Tổng thân tab: **802 dòng**. Ghi chú cấu trúc:
- Body/Combat **không** có `BeginChild` — chỉ là content thường.
- Visual/Teleport/World/Server/Tools/Misc có `BeginChild(...)`→`EndChild()` **bên trong** khối
  ⇒ cắt nguyên khối là stack ImGui vẫn cân bằng.
- Misc là nhánh `} else {` (không phải `else if`), kết thúc ở dòng 6086.
- Có đúng **3 static function-local** trong thân tab, move nguyên khối là giữ nguyên hành vi:
  `jmName` (5351, Body), `lastTpDiag` (5581, Teleport), `profSel` (6033, Misc).
- Đã đối chiếu **184 khai báo biến trong `main()` (dòng 3600–5131)** với text 8 tab:
  ngoài `cfg*`/`g_*`, chỉ có đúng các biến ở cột "main-local" là dùng thật
  (`frame`/`running` chỉ nằm trong chữ/comment — bỏ qua).

### 2.3 Helper inventory (snapshot)

| Dòng | Symbol | Ghi chú |
|---|---|---|
| 46 | `struct HitRect` | chuyển sang header |
| 47 | `static std::vector<HitRect> g_menuHits` | **định nghĩa** chuyển sang `ovl_ui.cpp` |
| 48 | `hitMark()` | dùng `g_menuHits` |
| 65 | `parseKey(const std::string&)` | pure |
| 95 | `static bool upsertCfgKey(...)` (forward decl) | **giữ** định nghĩa ở overlay.cpp (file IO) |
| 97/106 | `chkInt` (int* / bool*) | 2 overload |
| 110 | `sliderFloatCfg(...)` | gọi `upsertCfgKey` |
| 121 | `sliderIntCfg(...)` | gọi `upsertCfgKey` |
| 133–164 | `keyName(int)` | |
| 166–178 | `keyStateLabel(int,bool)` | gọi `keyName` |
| (~4230) | `keyCfgName(int)` | **giữ** ở overlay.cpp (capture poll dùng, tab không dùng) |
| 648–651 | `g_capVk`, `g_capCfg`, `g_capT0`, `g_capDrained` | dùng bởi `keyBindWidget` **và** capture poll trong main loop ⇒ định nghĩa chuyển `ovl_ui.cpp`, extern ở header |
| 684 | `keyBindWidget(...)` | dùng `g_cap*`, `g_diagState`, `keyName` |
| 717 | `markChildArea()` | dùng `g_menuHits` |
| 829–830 | `g_toast[96]`, `g_toastTick` | footer đọc ⇒ định nghĩa ở `ovl_ui.cpp`, extern |
| 831 | `toast(...)` | |
| 871 | `static const char* g_colHover` | giữ định nghĩa ở overlay.cpp, chỉ bỏ `static` |

### 2.4 Biến global dùng bởi tab — **87 identifier, tất cả đang `static`**

- 70 × `cfg*`, 17 × `g_*` (danh sách đầy đủ: Appendix A).
- Tất cả đều **định nghĩa ở file-scope trong overlay.cpp** (đã kiểm 87/87).
- Thêm 4 biến nội bộ helper phải extern: `g_capVk`, `g_capCfg`, `g_capT0`, `g_capDrained`;
  và 2 biến toast: `g_toast`, `g_toastTick` (2 biến này move hẳn sang `ovl_ui.cpp`).

### 2.5 Struct phải move sang header

| Struct | Dòng | Vì sao |
|---|---|---|
| `HitRect` | 46 | `g_menuHits` |
| `Ent` | 3889–3901 | Teleport/Body dùng `const std::vector<Ent>&` |
| `Waypoint` | 842 | `g_ways` |
| `SrvRow` | 2186–2192 | `SrvWire` |
| `SrvJoin` | 2194–2197 | Server tab dùng `SrvJoin jr` |
| `SrvState` | 2392–2414 | `srv` |
| `SrvWire` | 2418–2428 | `g_srvWire` |

---

## 3. Kiến trúc đích

### 3.1 Cây file

```
src\
  overlay.cpp              ← khung menu + main loop + aim/ESP/entity/server worker/cfg IO
  ovl\
    ovl_types.h            ← HitRect, Waypoint, Ent, SrvRow, SrvJoin, SrvState, SrvWire
    ovl_ui.h               ← helper UI + toast + keybind capture + extern g_menuHits/g_toast
    ovl_ui.cpp             ← định nghĩa helper + g_menuHits/g_toast/g_cap*
    ovl_state.h            ← extern 87 biến + 12 hàm (do overlay.cpp sở hữu)
    ovl_tabs.h             ← MenuCtx + 8 prototype drawTab*
    tab_body.cpp           (98 dòng gốc)
    tab_combat.cpp         (54)
    tab_visual.cpp         (92)
    tab_teleport.cpp       (190)
    tab_world.cpp          (93)
    tab_server.cpp         (165)
    tab_tools.cpp          (7)
    tab_misc.cpp           (103)
```

**13 file mới** (4 header + `ovl_ui.cpp` + 8 `tab_*.cpp`).
`overlay.cpp` giữ: khung menu (5132–5285, 6088–6154), dispatch 1 dòng/tab, toàn bộ phần còn lại.

### 3.2 Ai sở hữu gì (sau S1)

| File | Sở hữu | Ai đọc khi sửa tab |
|---|---|---|
| `tab_world.cpp` | UI tab World | **chỉ file này** + 4 header |
| `tab_server.cpp` | UI tab Server (không gồm worker HTTP) | file này + `ovl_types.h` |
| `overlay.cpp` | khung menu, footer, main loop, aim/ESP, cfg IO, server worker | chỉ khi sửa khung/hành vi |
| `ovl_ui.cpp` | helper widget + toast | hiếm khi sửa |
| `ovl_state.h` | danh sách state chia sẻ | khi thêm biến cfg mới |

### 3.3 Include graph

```
overlay.cpp
  #include "ovl/ovl_types.h"
  #include "ovl/ovl_state.h"
  #include "ovl/ovl_ui.h"
  #include "ovl/ovl_tabs.h"

ovl_ui.cpp        → ovl_ui.h, ovl_state.h (upsertCfgKey, g_diagState), imgui.h
tab_*.cpp         → ovl_tabs.h, ovl_ui.h, ovl_state.h, imgui.h
ovl_tabs.h        → sdk.h (Memory/Offsets), ovl_types.h, <vector>, <windows.h>
ovl_ui.h          → ovl_types.h, <string>, <windows.h>
ovl_state.h       → ovl_types.h, <string>, <vector>
ovl_types.h       → sdk.h (Vec3), <string>, <vector>, <set>, <mutex>, <cstdint>
```

Không cần thêm `-I`: `-Isrc` đã có (include kiểu `"ovl/ovl_ui.h"`), còn trong `src\ovl\`
thì include sibling trực tiếp (`"ovl_ui.h"`).

---

## 4. Hợp đồng dữ liệu (viết đúng thế này)

### 4.1 `ovl_tabs.h`

```cpp
#pragma once
#include "sdk.h"        // Memory, Offsets, SDK
#include "ovl_types.h"
#include <vector>
#include <windows.h>

// Bó biến main-local mà thân tab cần. Giá trị được chụp ngay trước dispatch,
// đúng thời điểm code inline cũ đọc chúng -> không đổi ngữ nghĩa.
struct MenuCtx {
    Memory& m;
    const Offsets& o;
    const std::vector<Ent>& ents;
    uint64_t place = 0;
    bool haveWrite = false;
    bool aimOn = false;       // main(): aim toggle/hold state
    bool trigOn = false;
    bool acOn = false;
    bool flyApply = false;    // main(): cfg && hotkey (effective)
    bool speedApply = false;
    bool jumpApply = false;
    int dbgEnts = 0;
    float loopMs = 0.0f;
    DWORD nowTick = 0;   // = nowLoop trong main loop
};

void drawTabBody(MenuCtx& c);
void drawTabCombat(MenuCtx& c);
void drawTabVisual(MenuCtx& c);
void drawTabTeleport(MenuCtx& c);
void drawTabWorld(MenuCtx& c);
void drawTabServer(MenuCtx& c);
void drawTabTools(MenuCtx& c);
void drawTabMisc(MenuCtx& c);
```

### 4.2 `ovl_ui.h` (rút gọn)

```cpp
#pragma once
#include "ovl_types.h"
#include <string>
#include <windows.h>

extern std::vector<HitRect> g_menuHits;
extern char g_toast[96];
extern DWORD g_toastTick;
extern int* g_capVk;
extern const char* g_capCfg;
extern DWORD g_capT0;
extern bool g_capDrained;

void hitMark();
void markChildArea();
void toast(const char* fmt, ...);
int  parseKey(const std::string& raw);
std::string keyName(int vk);
void keyStateLabel(int vk, bool applying);
void keyBindWidget(const char* label, int* vk, const char* cfgKey, float w = 150.0f);
bool chkInt(const char* label, int* v);
bool chkInt(const char* label, bool* v);
bool sliderFloatCfg(const char* label, float* v, float lo, float hi,
                    const char* fmt, const char* cfgKey);
bool sliderIntCfg(const char* label, int* v, int lo, int hi,
                  const char* fmt, const char* cfgKey);

// định nghĩa ở overlay.cpp (file IO + exePath) — S2 mới move
bool upsertCfgKey(const char* key, const char* value);
```

### 4.3 `ovl_state.h` (mẫu; Appendix B sinh tự động)

```cpp
#pragma once
#include "ovl_types.h"
#include <string>
#include <vector>

// ---- cfg (overlay.cpp sở hữu) ----
extern int cfgMenuTab;
extern int cfgEspOn;
extern float cfgAimSmoothing;
/* ... 70 dòng ... */

// ---- runtime state ----
extern bool g_diagState;
extern const char* g_colHover;
extern int g_toggleVk;
extern SrvState srv;
extern SrvWire g_srvWire;
extern std::vector<Waypoint> g_ways;
extern char g_tpPosName[48];
extern Vec3 g_tpTargetPos;
/* ... */

// ---- hàm overlay.cpp cung cấp cho tab ----
std::string srvShort(const std::string& j);
std::vector<SrvRow> srvFilterRows(const std::vector<SrvRow>& in,
                                  const std::string& curJob);
void srvRequestList(uint64_t place);
void srvOpenJoin(uint64_t place, const std::string& job, const char* why);
bool srvClipGet(std::string& out);
bool srvClipSet(const std::string& s);
bool srvParseJoin(const std::string& raw, uint64_t curPlace, SrvJoin& out);
std::string srvBuildLink(uint64_t place, const std::string& job);
bool profileExists(const std::string& slot);
bool saveProfileSlot(const std::string& slot);
bool loadProfileSlot(const std::string& slot);
void setPlaceProfile(uint64_t place, const std::string& slot);
```

### 4.4 Dispatch mới trong `overlay.cpp`

Thay nguyên khối 5286–6086 bằng:

```cpp
MenuCtx tabCtx{ m, o, ents, place, haveWrite,
                aimOn, trigOn, acOn,
                flyApply, speedApply, jumpApply,
                dbgEnts, loopMs, nowLoop };
if      (cfgMenuTab == 0) drawTabBody(tabCtx);
else if (cfgMenuTab == 1) drawTabCombat(tabCtx);
else if (cfgMenuTab == 2) drawTabVisual(tabCtx);
else if (cfgMenuTab == 3) drawTabTeleport(tabCtx);
else if (cfgMenuTab == 4) drawTabWorld(tabCtx);
else if (cfgMenuTab == 5) drawTabServer(tabCtx);
else if (cfgMenuTab == 6) drawTabTools(tabCtx);
else                      drawTabMisc(tabCtx);
```

Lưu ý thứ tự khởi tạo aggregate phải đúng thứ tự field trong struct
(`m, o, ents, place, haveWrite, aimOn, trigOn, acOn, flyApply, speedApply, jumpApply,
dbgEnts, loopMs, nowTick`).

Mỗi `tab_*.cpp`:

```cpp
#include "ovl_tabs.h"
#include "ovl_ui.h"
#include "ovl_state.h"
#include "imgui.h"

void drawTabWorld(MenuCtx& c) {
    // ---------------- World: lighting / atmosphere / terrain ----------------
    ... nguyên văn thân tab, đã bỏ wrapper if/else ...
}
```

Tab không dùng `c` thì để nguyên; nếu compiler cảnh báo unused parameter thì thêm `(void)c;`.

---

## 5. Các bước triển khai (theo thứ tự, có gate)

### S1.0 — Snapshot + baseline (10 phút)
1. Backup:
   ```
   backup\overlay_S1PRE_20261020.cpp
   backup\ext_overlay_S1PRE_20261020.exe
   backup\build_fast_S1PRE.bat
   backup\build_S1PRE.bat
   ```
2. Build baseline: `build_fast.bat` → ghi exit code + size.
3. `ext_overlay.exe -aimtest` → exit 0.
4. Đo baseline hits 8 tab (Appendix C) → `backup\s1_baseline_hits.txt`.
   **Điều kiện:** game (`RobloxPlayerBeta`) đang chạy và **không** có `ext_overlay.exe`
   nào đang chạy (mutex singleton).

**Gate:** baseline có đủ 8 dòng `menu tab=N ... hits=M`.

### S1.1 — Tạo `src\ovl\` + đủ 13 file + sửa build (20 phút)
1. Tạo `ovl_types.h`, move `HitRect`, `Waypoint`, `Ent`, `SrvRow`, `SrvJoin`, `SrvState`, `SrvWire`.
2. `overlay.cpp`: xoá các định nghĩa đã move, thêm `#include "ovl/ovl_types.h"`.
3. Tạo `ovl_ui.h`, `ovl_state.h`, `ovl_tabs.h` (nội dung theo §4, phần chưa dùng để trống).
4. Tạo `ovl_ui.cpp` + 8 `tab_*.cpp` **rỗng** (chỉ `#include` header cần thiết).
5. Cập nhật **ngay** `build_fast.bat` + `build.bat` với đủ 9 file `.cpp` mới (§6), để các
   bước sau không phải sửa build nữa.

**Gate:** build exit 0 (9 file rỗng + overlay.cpp); `-aimtest` 0; menu vẫn vẽ.

### S1.2 — Move helpers sang `ovl_ui.cpp` (30 phút)
1. Tạo `ovl_ui.cpp`, move nguyên văn: `hitMark`, `parseKey`, `chkInt`×2, `sliderFloatCfg`,
   `sliderIntCfg`, `keyName`, `keyStateLabel`, `keyBindWidget`, `markChildArea`, `toast`.
2. Move định nghĩa `g_menuHits`, `g_toast`, `g_toastTick`, `g_capVk`, `g_capCfg`,
   `g_capT0`, `g_capDrained` sang `ovl_ui.cpp`.
3. `overlay.cpp`: xoá forward decl `static bool upsertCfgKey(...)` (dòng 95);
   đổi định nghĩa `upsertCfgKey` thành non-static (giữ nguyên thân).
4. `ovl_ui.h`: khai báo đúng như §4.2.

**Gate:** build 0 lỗi; `-aimtest` 0; đo lại hits 8 tab == baseline; thử trong game:
kéo menu, tick 1 checkbox, bấm 1 slider, bấm bind phím (Misc), mở color popup (Visual).

### S1.3 — Extern sweep 87 biến (30 phút)
1. Chạy script Appendix B → sinh `ovl_state.h` (dry-run in ra file).
2. Bỏ `static` ở **86** định nghĩa còn lại trong `overlay.cpp` (87 trừ `g_menuHits` đã
   move ở S1.2 — script in đúng số, phải ra 86).
3. Thêm 12 prototype hàm vào `ovl_state.h`; bỏ `static` ở 12 định nghĩa tương ứng.

**Gate:** build 0 lỗi. Lỗi `'static' declaration follows non-static` = quên bỏ `static`;
lỗi `undefined reference` = thiếu extern.

### S1.4 — MenuCtx + dispatch (10 phút)
1. Viết `ovl_tabs.h` (§4.1) — 8 file `tab_*.cpp` đã tạo rỗng từ S1.1.
2. Compile thử với prototype (chưa ai gọi) → phải xanh, dispatch cũ vẫn chạy.

**Gate:** build 0 lỗi (dispatch cũ vẫn chạy).

### S1.5–S1.12 — Tách từng tab (thứ tự rủi ro tăng dần)

| Bước | Tab | Dòng gốc | Dependency | Thời gian |
|---|---|---|---|---|
| S1.5 | Tools | 7 | không | 5' |
| S1.6 | World | 93 | không | 10' |
| S1.7 | Combat | 54 | `c.haveWrite`, `c.aimOn/trigOn/acOn` | 10' |
| S1.8 | Body | 98 | `c.m/o/ents`, `c.flyApply/speedApply/jumpApply` | 15' |
| S1.9 | Visual | 92 | `g_colHover` (extern) | 15' |
| S1.10 | Misc | 103 | `c.place/dbgEnts/o/loopMs`, profile fns | 20' |
| S1.11 | Server | 165 | `c.place`, `c.nowTick`, `srv*`, `srv` | 25' |
| S1.12 | Teleport | 190 | `c.*`, `toast`, `g_*tp*`, `g_ways` | 25' |

Quy trình mỗi tab:
1. Cắt thân tab (giữa `{` mở và `} else`/`}` đóng) → dán nguyên văn vào `tab_<x>.cpp`.
2. Trong `overlay.cpp` thay nhánh bằng đúng 1 dòng gọi `drawTab<X>(tabCtx);`.
3. Build; nếu lỗi thiếu symbol → bổ sung vào `ovl_state.h` (chỉ khai báo, không move code).
4. Chạy nhanh: `-diag -nohide -t 3` với `menu.tab=N` → phải thấy `hits=` > 0 và không crash.
5. Cuối mỗi bước: **backup exe** nếu muốn có mốc rollback từng phần.

**Gate mỗi tab:** build 0 lỗi + diag đúng tab đó + không crash 10 giây.

### S1.13 — Build scripts: kiểm tra lần cuối (10 phút)
`build_fast.bat` dòng 10 và `build.bat` dòng 25 đã được sửa từ S1.1 (§6). Bước này chỉ:
1. Diff 2 dòng lệnh overlay — danh sách nguồn phải **giống hệt nhau**.
2. Chạy `build_fast.bat` từ `C:\Windows\System32` (test không phụ thuộc CWD).
3. Chạy `build.bat` một lần đầy đủ (build cả ~25 tool) — không được vỡ tool nào.

**Gate:** cả 2 exit 0; `-aimtest` 0.

### S1.14 — Verify tổng + docs (30 phút)
1. So hits 8 tab với baseline (Appendix C) — **phải khớp từng số**.
2. Smoke test trong game theo checklist §7.3.
3. Viết `HANDOFF_S1_split_menu.md`: file map, số dòng trước/sau, hits baseline vs after,
   build command, điều cần biết khi thêm tab mới.
4. (Tuỳ chọn) `OVERLAY_ARCH.md` ở root: module map + luật "tab mới = file mới
   + 1 dòng dispatch + 1 dòng trong build".

**Gate:** §10 Definition of Done.

---

## 6. Build scripts — sửa chính xác

### `build_fast.bat` dòng 10 (thay dòng hiện tại — 1 dòng, không dùng `^`)

```bat
  "%GCC%" -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp src\ovl\ovl_ui.cpp src\ovl\tab_body.cpp src\ovl\tab_combat.cpp src\ovl\tab_visual.cpp src\ovl\tab_teleport.cpp src\ovl\tab_world.cpp src\ovl\tab_server.cpp src\ovl\tab_tools.cpp src\ovl\tab_misc.cpp lib\libimgui.a -I%IMGUI% -I%IMGUI%\backends -Isrc -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
```

### `build.bat` dòng 25 — cùng danh sách nguồn, giữ nguyên flags hiện có.

Lý do danh sách tường minh: `src\` có ~25 tool standalone có `main()` riêng; glob sẽ
gây trùng `main`/symbol. Khi thêm tab mới sau này: thêm 1 file vào **cả 2** script.

---

## 7. Verification protocol

### 7.1 Build
- `build_fast.bat` exit 0, không warning mới (so số warning trước/sau).
- `ext_overlay.exe -aimtest` exit 0.

### 7.2 Regression hit-test theo tab (tự động, không cần click)

Vì cfg resolve cạnh exe, mỗi tab test bằng một sandbox riêng (script Appendix C):
- Copy `ext_overlay.exe` + `overlay.cfg` (sửa `menu.tab=N`, `menu.show=1`, `menu.collapsed=0`)
  vào `%TEMP%\opencode\s1\tabN\`.
- Chạy `ext_overlay.exe -diag -nohide -t 4`, lọc dòng `menu tab=N ... hits=...`.
- So với baseline: **cùng `hits=`**, cùng `size=`; `pos=` có thể lệch nếu cfg copy khác
  (dùng đúng 1 file cfg gốc cho cả 8 lần chạy để loại biến).

Tiêu chí đạt: 8/8 tab khớp `hits` và `size`; không tab nào `hits=0`.

### 7.3 Smoke test trong game (user làm, ~3 phút)
1. Mở menu (INSERT) → kéo title bar → thả: vị trí lưu vào `overlay.cfg`.
2. Bấm lần lượt 8 tab: tab nào cũng render, không nhấp nháy, không mất menu.
3. Visual: mở 1 color popup → click trong popup không xuyên xuống game; click ngoài đóng popup.
4. Teleport: scroll list player → camera không zoom (markChildArea hoạt động).
5. Misc: bấm 1 bind phím → `[press key]` → bấm phím → lưu đúng vào cfg.
6. Combat: tick Trigger/Aim → trạng thái ON/off hiển thị đúng.
7. Panic (DELETE) vẫn tắt menu như cũ (hành vi đã biết: set `cfgMenuShow=0` trong RAM).

### 7.4 Không hồi quy hành vi
- Không đổi tên/không thêm/bớt cfg key.
- Không đổi thứ tự tab, kích thước menu, vị trí widget.
- `menu tab=...` diag in đúng như trước.

---

## 8. Rủi ro & cách chặn

| Rủi ro | Xác suất | Chặn |
|---|---|---|
| Thiếu extern → link error | Cao | Script Appendix B + build sau từng tab; lỗi `undefined reference` chỉ đúng tên |
| Quên bỏ `static` | Cao | Compiler báo `'static' declaration follows non-static` tại dòng định nghĩa |
| Mất cân bằng BeginChild/EndChild | Trung bình | Move verbatim; đếm `BeginChild`==`EndChild` trong mỗi file mới |
| Tab dùng biến main-local bị bỏ sót | Trung bình | Bảng §2.2 đã đo; compiler + diag bắt tiếp |
| `keyBindWidget` mất capture state | Thấp | `g_cap*` move cùng file, extern đúng |
| Popup color picker không nhận click | Thấp | Popup hit-test nằm ở khung (giữ nguyên), không move |
| Chat khác sửa overlay.cpp song song | Trung bình | Freeze feature work trong lúc S1; backup trước; nếu có sửa thì rebase thủ công từng tab |
| Chạy 2 overlay → mutex exit 1 | Thấp | Kiểm `Get-Process ext_overlay` trước khi test |
| Build script quên file mới | Thấp | Chạy `build_fast.bat` từ CWD khác; lỗi `undefined reference to drawTab*` |

**Không có rủi ro hành vi** nếu tuân thủ move verbatim: không sửa 1 dòng logic nào.

---

## 9. Rollback

- Toàn bộ: copy `backup\overlay_S1PRE_20261020.cpp` → `src\overlay.cpp`,
  `backup\build_*_S1PRE.bat` → root, xoá `src\ovl\`, build lại.
- Từng phần: mỗi tab là 1 commit tinh thần — giữ backup exe sau mỗi bước nếu muốn lùi cục bộ.
- Không có thay đổi nào trong `overlay.cfg` / `offsets.h` / `sdk.h` ⇒ rollback không cần
  migrate dữ liệu.

---

## 10. Definition of Done

1. `src\ovl\` có đủ 4 header + 1 ui.cpp + 8 tab.cpp; mỗi tab 1 hàm `drawTab*`.
2. `overlay.cpp` **không còn** thân tab nào; dispatch chỉ 8 dòng gọi hàm.
3. `grep "cfgMenuTab ==" overlay.cpp` chỉ còn trong dispatch (8 dòng).
4. Build `build_fast.bat` + `build.bat` exit 0; `-aimtest` exit 0.
5. Hits 8 tab khớp baseline 8/8.
6. Smoke test §7.3 pass.
7. `overlay.cpp` ≤ 6.500 dòng (dự kiến ~6.280).
8. `HANDOFF_S1_split_menu.md` ghi: file map, số đo trước/sau, cách thêm tab mới.

---

## 11. Ước lượng & lợi ích

| Việc | Thời gian |
|---|---|
| S1.0 baseline | 10' |
| S1.1 file + build | 20' |
| S1.2 helpers | 30' |
| S1.3 extern sweep | 30' |
| S1.4 ctx | 10' |
| 8 tab | ~2h05' |
| Build scripts + verify + docs | 70' |
| **Tổng** | **~4 giờ** (1 phiên làm việc) |

Lợi ích sau S1 (đo bằng số dòng phải đọc):

| Task | Trước | Sau |
|---|---|---|
| Sửa tab World | 7.217 dòng | ~93 + ~300 header ≈ **390** |
| Sửa tab Tools | 7.217 | ~7 + 300 ≈ **307** |
| Sửa tab Teleport | 7.217 | ~190 + 300 ≈ **490** |
| Sửa tab Server | 7.217 | ~165 + 300 + `ovl_types.h` ≈ **500** |
| Sửa khung menu | 7.217 | 7.217 − 802 (tab) ≈ **6.400** |

Giảm 15–23× cho task theo tab. Nếu sau này AI đọc cả file vì không biết tab nào →
`ovl_tabs.h` là bản đồ: tên tab → file.

---

## 12. Sau S1 (không làm bây giờ)

- **S2**: tách `ovl_cfg.cpp` (parse/save ~600 dòng), `ovl_world.cpp` (~480), `ovl_hitbox.cpp`
  (~250), `ovl_server.cpp` (worker HTTP ~500). `overlay.cpp` còn ~4.000.
- **S3 (tuỳ chọn)**: aim/ESP — rủi ro cao vì 14 lambda `[&]` trong `main()`; chỉ làm nếu
  S1+S2 chứng minh quy trình an toàn.

---

## Appendix A — 87 identifier tab dùng (snapshot)

```
cfgACEnabled cfgACKey cfgAimEnabled cfgAimFov cfgAimKey cfgAimSmoothing cfgAutoProfile
cfgEspBox cfgEspColEnemy cfgEspColNpc cfgEspColTeam cfgEspDist cfgEspFill cfgEspHead
cfgEspHp cfgEspLine cfgEspMaxDist cfgEspName cfgEspNpc cfgEspOn cfgEspOof cfgEspSkel
cfgFlyEnabled cfgFlyKey cfgFlySpeed cfgHitboxEnabled cfgHudFeats cfgHudOn cfgMenuTab
cfgMenuUnlockMouse cfgMenuX cfgMenuY cfgPanicKey cfgPlyJump cfgPlyJumpKey cfgPlyJumpMode
cfgPlyJumpVal cfgPlyMode cfgPlySpeed cfgPlySpeedKey cfgPlySpeedVal cfgRadarOn
cfgRadarRange cfgRadarSize cfgSrvAfkMin cfgSrvAntiAfk cfgSrvAutoRejoin cfgSrvGeo
cfgSrvHopFresh cfgSrvHopMin cfgTpNpc cfgTrigEnabled cfgTrigKey cfgTrigKeyMode cfgWda
cfgWorldAtmo cfgWorldBright cfgWorldClock cfgWorldClockVal cfgWorldExpo cfgWorldExpoVal
cfgWorldFog cfgWorldFov cfgWorldFovVal cfgWorldFull cfgWorldFx cfgWorldGrass
cfgWorldShadows cfgWorldWater cfgWorldZoom
g_colHover g_diagState g_flyActive g_jumpActive g_menuHits g_menuPosForce g_plyCurSpeed
g_speedActive g_srvSel g_srvWire g_toggleVk g_tpPosName g_tpPosTick g_tpRequestTick
g_tpTargetPos g_tpTargetPrim g_ways
```

Ngoài danh sách: `g_capVk/g_capCfg/g_capT0/g_capDrained/g_toast/g_toastTick`
(move định nghĩa sang `ovl_ui.cpp`).

## Appendix B — Script sinh `ovl_state.h` (PowerShell, chạy tại root repo)

```powershell
# Sinh extern cho các biến còn định nghĩa ở overlay.cpp sau S1.2.
# Dry-run: chỉ in; thêm -Apply để bỏ 'static' ngay trong overlay.cpp.
param([switch]$Apply)
$ids  = @(Get-Content 'tools\s1_ids.txt')   # 1 identifier / dòng (Appendix A)
$f    = 'src\overlay.cpp'
$lines = Get-Content $f
$decls = @()
$edits = @()
foreach ($id in $ids) {
  for ($i = 0; $i -lt $lines.Count; $i++) {
    $pat = '^\s*static\s+(.+?)\s+' + [regex]::Escape($id) +
           '\s*((?:\[[^\]]*\])?)\s*(?:=.*?|\{[^}]*\})?;\s*(?://.*)?$'
    if ($lines[$i] -match $pat) {
      $type  = $Matches[1]
      $array = $Matches[2]
      $decls += "extern $type $id$array;"
      $edits += $i
      break
    }
  }
}
"// sinh tự động — $($decls.Count) biến" | Set-Content 'src\ovl\ovl_state.h' -Encoding utf8
$decls | Add-Content 'src\ovl\ovl_state.h' -Encoding utf8
if ($Apply) {
  foreach ($i in $edits) { $lines[$i] = $lines[$i] -replace '^(\s*)static\s+', '$1' }
  Set-Content $f $lines -Encoding utf8
}
"decls=$($decls.Count) edits=$($edits.Count)"
```

> **Đã test pattern này trên snapshot:** khớp **87/87** identifier (kể cả dòng có comment
> cuối và `Vec3 g_tpTargetPos{}`). Sau S1.2, `g_menuHits/g_toast/g_toastTick/g_cap*` đã
> move sang `ovl_ui.cpp` ⇒ script chỉ khớp **86** biến. Nếu ra ít hơn 86: dừng, kiểm tra
> trước khi `-Apply`. Prototype 12 hàm (§4.3) thêm tay vào cuối header.
> **Chạy đúng 1 lần ở S1.3** — chạy lại sẽ ghi đè `ovl_state.h` và mất phần prototype thêm tay.

## Appendix C — Script đo hits 8 tab (baseline & after)

```powershell
# Yêu cầu: không có ext_overlay.exe đang chạy; RobloxPlayerBeta đang chạy.
if (Get-Process ext_overlay -ErrorAction SilentlyContinue) {
  throw 'close ext_overlay.exe first (singleton mutex)'
}
$exe  = 'ext_overlay.exe'
$src  = 'overlay.cfg'
$base = Join-Path $env:TEMP 'opencode\s1hits'
$out  = 'backup\s1_hits_after.txt'   # baseline: s1_baseline_hits.txt
Remove-Item $out -ErrorAction SilentlyContinue
foreach ($n in 0..7) {
  $d = Join-Path $base "tab$n"
  New-Item -ItemType Directory -Force $d | Out-Null
  Copy-Item $exe $d -Force
  if (Test-Path 'offsets_cache.json') { Copy-Item 'offsets_cache.json' $d -Force }
  (Get-Content $src) -replace '^menu\.tab=.*$', "menu.tab=$n" `
                     -replace '^menu\.show=.*$', 'menu.show=1' `
                     -replace '^menu\.collapsed=.*$', 'menu.collapsed=0' |
    Set-Content (Join-Path $d 'overlay.cfg') -Encoding ascii
  Push-Location $d
  $txt = (& .\ext_overlay.exe -diag -nohide -t 4 2>&1 | Out-String)
  Pop-Location
  $line = ($txt -split "`r?`n" | Where-Object { $_ -match "menu tab=$n " } |
           Select-Object -Last 1)
  "tab$n  $line" | Tee-Object -FilePath $out -Append
}
```

Chạy script này **trước** khi sửa (baseline) và **sau** S1.14, diff 2 file:
`hits=` và `size=` phải giống hệt; `pos=` bỏ qua.

## Appendix D — Danh sách file mới (13)

```
src\ovl\ovl_types.h        (header)
src\ovl\ovl_ui.h           (header)
src\ovl\ovl_ui.cpp
src\ovl\ovl_state.h        (header)
src\ovl\ovl_tabs.h         (header)
src\ovl\tab_body.cpp
src\ovl\tab_combat.cpp
src\ovl\tab_visual.cpp
src\ovl\tab_teleport.cpp
src\ovl\tab_world.cpp
src\ovl\tab_server.cpp
src\ovl\tab_tools.cpp
src\ovl\tab_misc.cpp
```

4 header + 9 cpp = **13 file**.
