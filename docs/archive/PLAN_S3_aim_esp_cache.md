# PLAN S3 — tách entity cache / ESP / aim khỏi `src\overlay.cpp`

> **Snapshot đo lúc:** 2026-10-20, sau S2POST.
> `src\overlay.cpp` = **4.085 dòng / 196.206 B**; SHA-256 `5DAA89C05A605D9100636F1006B45EF7675145F0DCC6DD761AAC1CC1103CA9E4`;
> CRLF=0, BOM=false. `ext_overlay.exe` hiện tại: SHA-256 `4CA03C880D29F6673BC651A9ECDEA377CC48025A2C65235CBE9D833B88D22F00`.
> Mọi số dòng dưới đây chỉ để định vị. **Khi implement luôn tìm bằng marker**, không bám số dòng cứng.
> Chưa sửa source nào. Plan này cần được duyệt trước khi làm S3.1.

---

## 0. TL;DR

Tách 3 khối lớn còn lại của `overlay.cpp` thành module riêng, **move verbatim, không đổi hành vi**:

| Bước | Khối | Dòng hiện tại | Move | Đích | `overlay.cpp` sau |
|---|---|---|---:|---|---:|
| S3.0 | snapshot + baseline | — | — | `backup\S3PRE_20261020\` | 4.085 |
| S3.1 | entity cache 3 tầng | 1.582–1.887 | **306** (299 cache + 7 `invalidate` hoist) | `src\ovl\ovl_cache.cpp` | ~3.780 |
| S3.2 | ESP + radar | 3.060–3.283 | **224** | `src\ovl\ovl_esp.cpp` | ~3.550 |
| S3.3a | aim tick + trigger + autoclick + viewport restore | 3.285–3.999, 4.008–4.050, 1.910–1.926 | **~780** | `src\ovl\ovl_aim.cpp` | ~2.730 |
| S3.3b *(optional)* | velext math + writer thread + `-aimtest` | 60–140, 177–362, 797–816, 907–1.103, 1.106–1.307 | **~650** | `src\ovl\ovl_aim.cpp` | ~2.130 |
| S3.4 | verify + docs + snapshot | — | — | `HANDOFF_S3_aim_esp_cache.md` | — |

**Khuyến nghị:** làm **full core S3.1 → S3.2 → S3.3a → S3.4** (~8–9 h). S3.3b làm sau nếu muốn `overlay.cpp` xuống ~2.100 dòng; không bắt buộc để đạt mục tiêu S3.

**Không làm:** không đổi cfg key/default/thứ tự tab, không đổi aim math/hằng số, không đổi thứ tự draw ESP→aim, không đổi `frame`/viewport refresh, không glob `src\*.cpp`.

---

## 1. Trả lời 4 câu hỏi

### Q1 — Full S3 hay chỉ cache?

**Khuyến nghị: full core, không chỉ cache.**

| Phương án | Thời gian | `overlay.cpp` | Rủi ro | Khi nào chọn |
|---|---:|---:|---|---|
| Cache-only (S3.1 + S3.4) | ~2,5–3 h | ~3.780 | Thấp | Aim/ESP đang đóng băng, chỉ muốn hết hitch cache |
| **Full core (S3.1 + S3.2 + S3.3a + S3.4)** | **~8–9 h** | **~2.730** | **Trung bình** | **Mặc định — aim/ESP là phần sửa nhiều nhất** |
| Full + S3.3b | ~11–12 h | ~2.130 | Trung bình-cao | Muốn `ovl_aim.cpp` sở hữu trọn aim math + writer |

Lý do không cache-only: cache chỉ chiếm 306/4.085 dòng. Aim (715 dòng) + ESP (224) + trigger/autoclick (52) + restore viewport (17) vẫn nằm trong `main`, mỗi lần chỉnh aim vẫn phải mở file 4k dòng. Lịch sử project cho thấy aim được sửa liên tục (`HANDOFF_aimsmooth.md`, `velext_aim_findings.md`, các phase C). Tách aim là giá trị chính của S3.

Nếu chỉ có 2–3 h và muốn zero-risk: làm **S3.1 cache ngay**, hoãn S3.2/S3.3. Cache là dependency của ESP/aim nên làm trước vẫn có lợi.

### Q2 — Thứ tự cache → ESP → aim? Trigger/autoclick có tách riêng?

**Thứ tự: S3.1 cache → S3.2 ESP → S3.3a aim.** Lý do:

1. **Cache trước** vì `ents` là đầu vào của cả ESP và aim; cache không có draw/input nên dễ verify bằng số lượng entity + `aim_diag.log`.
2. **ESP trước aim** vì ESP chỉ đọc `ents`/`vm`, không ghi state; nó xác nhận cache đúng (box/name/team/HP/skeleton) trước khi aim dùng cùng dữ liệu.
3. **Aim sau cùng** vì consume cả `ents` lẫn `vm`, có nhiều state nhất, và là phần rủi ro nhất.

**Trigger: giữ trong `AimCtx::tick`, không tách TU.** Trigger đọc trực tiếp `bestTarget`, `haveTarget`, `ccx/ccy`, `dl` — tách ra sẽ phải tạo `TrigCtx` 8–10 field và chèn call giữa selection và draw, trong khi logic chỉ 37 dòng. Giữ nguyên thứ tự: select → act → draw line/circle → trigger → trigger circle.

**Autoclick: method riêng `AimCtx::autoclickTick`, cùng TU `ovl_aim.cpp`.** Nó độc lập với target và chạy ngoài `if (haveVm)`, nên tách method là đúng biên; tách TU riêng chỉ thêm header/context cho 43 dòng, không có lợi.

**Không tách `ovl_trigger.cpp`.**

### Q3 — Main-local capture chính xác?

Chi tiết đầy đủ ở **§5**. Tóm tắt thiết kế:

- **Ref bundle + owned state**, không dùng `static` local, không copy giá trị.
- `EntityCtx`: 6 ref (`m, sdk, o, ents, uisAddr, stickPlayer`), sở hữu `ecache/classCache/cDm/cPlayers/lastPlaceSeen/npcScanSkip`.
- `EspCtx`: 10 ref (`m, o, ents, vpx, vpy, vpw, vph, sw, sh, camAddr`), không sở hữu state.
- `AimCtx`: **17 ref** + **32 state owned**. Các state chỉ aim dùng (`accX/accY`, `ema*`, `mouseCtl`, `bp*`, `trigPhase`, `ac*`, `lock*`, `vpShifted/shiftedNow`, `dbgEnts`…) chuyển thành member; các state main/cache còn đọc (`stickPlayer`, `uisAddr`) hoặc main ghi (`aimOn/trigOn/acOn`) giữ ref.
- `haveVm`, `dl`, `vm`, `fgPid`, `typingNow` là **parameter** của `tick`, không phải member.
- `frame` **không move**; viewport refresh 3.037–3.045 ở lại main (xem §2).
- `invalidateAimSnapshot` thành **free function** (S3.1), `AimSnap/g_snap*` export qua `ovl_aim.h`.
- `MouseCtlState`, `VelextSens`, block hằng số velext chuyển vào header.
- Cùng kiểu ref (6 `int`, 3 `bool`, 2 `uint64_t`) có **assert địa chỉ tạm** ngay sau construct để bắt swap; gỡ ở S3.4.

### Q4 — File map, dòng, gate, rollback?

- File map + ước lượng: **§3, §12**.
- Thứ tự implement + gate từng bước: **§6, §8**.
- Checklist in-game: **§9**.
- Rollback snapshot: **§11**.
- Definition of Done: **§13**.

---

## 2. Hiện trạng đo được

### 2.1 Ranh giới khối

| # | Khối | Marker bắt đầu | Marker kết thúc | Dòng | Ghi chú |
|---|---|---|---:|---|
| A | Entity cache | `// ---- 3-tier entity cache` (1.582) | `};` của `refreshEnts` (1.887) | **306** | 5 lambda: `classOf`, `resolveParts`, `clearResolvedParts`, `invalidateAimSnapshot` (block E), `refreshEnts` |
| B | `w2s` | `auto w2s = [&]` (1.888) | `};` (1.900) | 13 | dùng `vpx/vpy/vpw/vph/sw/sh` |
| C | `releaseTriggerIfDown` | `auto releaseTriggerIfDown = [&]` (1.901) | `};` (1.909) | 9 | dùng `trigPhase` |
| D | `restoreViewportIfNeeded` | `auto restoreViewportIfNeeded = [&]` (1.910) | `};` (1.926) | 17 | dùng `vpShifted/shiftedNow/camAddr/haveWrite` |
| E | `invalidateAimSnapshot` | `auto invalidateAimSnapshot = [&]` (1.655) | `};` (1.661) | 7 | dùng `g_snap*` |
| F | ESP + radar | `uint64_t espLocalTeam = 0;` (3.060) | `}` radar (3.283) | **224** | nằm trong `if (haveVm)` 3.059–4.000 |
| G | Aim + trigger | `// ---- Aimbot v2` (3.285) | `}` đóng `(wantAim||wantTrig)&&haveVm` (3.999) | **715** | `AimTarget` local; 2 lambda `[&]` (`evaluateEntity`, MouseService walk) |
| H | Autoclick | `// ---- Autoclicker` (4.008) | `}` (4.050) | 43 | chạy ngoài `if (haveVm)` |
| I | AimSnap + writer | `struct AimSnap` (797) | `timeEndPeriod(1); }` (1.103) | ~217 | bị cắt bởi `diagRate`/mouse-clip 819–905 |
| J | Quat helpers | `// ---- quaternion helpers` (60) | `}` `quatToMat` (140) | 81 | chỉ writer dùng |
| K | Velext math | `// ---- velext aim model` (177) | `}` `mouseCursorHidden` (362) | ~152 | trừ block hằng số 179–212 |
| L | `-aimtest` | `// ---- velext aim-model self-test` (1.106) | `}` (1.307) | 202 | pure math, không cần game |

> **Ghi chú audit:** khối aim 3.285–3.999 chỉ có **2 lambda `[&]`** (`evaluateEntity`, MouseService `walkChildren`), không phải 14. Con số ~14 là tổng lambda rải rác của cache (5, gồm `invalidateAimSnapshot`), ESP (3: `rgb`, `w2sPrim`, `w2sRay`), aim (2), `w2s`/`restore`/`release` (3). Vì vậy thiết kế dựa trên **ranh giới khối + state**, không dựa trên số lambda.

Kiểm chứng biên:

- 1.580 trống; 1.582 comment cache; 1.887 `};`; 1.888 `auto w2s`.
- 1.900 `};`; 1.901 `releaseTriggerIfDown`; 1.909 `};`; 1.910 `restoreViewportIfNeeded`.
- 3.058 `ImDrawList* dl`; 3.059 `if (haveVm) {`; 3.060 ESP start; 3.283 `}` radar; 3.284 trống; 3.285 aim comment.
- 3.997 `}` trigger circle; 3.998 `}` `if (wantTrig)`; 3.999 `}` `if ((wantAim||wantTrig)&&haveVm)`; 4.000 `}` `if (haveVm)`.
- 4.001 `restoreViewportIfNeeded(false)`; 4.003 hitbox tick; 4.008 autoclick comment; 4.050 `}`; 4.051 `restoreViewportIfNeeded(false)` (lần 2).
- 797 `struct AimSnap`; 819 `diagOpen`; 837 `diagRate`; 853 mouse-clip; 907 `buildAimMatrix`; 963 `writerThread`; 1.103 `}`; 1.106 `runAimSelfTest`; 1.307 `}`.

### 2.2 Các phát hiện quan trọng (đã audit)

1. **`frame` không bao giờ tăng.** Khai báo ở 1.516 nhưng không có `frame++`/`frame +=`. Vì vậy `if ((frame % 60) == 0)` ở 3.037 **luôn đúng**, viewport refresh chạy mỗi frame. **Giữ nguyên ở main**, không move vào ESP; nếu move và vô tình tăng `frame`, hành vi đổi.
2. **`uisAddr` được main đọc ngoài cache.** Chat-focus guard ở 1.944 dùng `uisAddr`. Vì vậy `uisAddr` **không thể** thuộc sở hữu riêng của `EntityCtx`; phải là ref.
3. **`stickPlayer` được main + cache cùng dùng.** Cache clear khi `GameLoaded=0` (1.672); key release clear (2.035); aim đọc/ghi (3.463–3.543). Phải là ref, không owned.
4. **`invalidateAimSnapshot` được gọi ngoài cache.** 1.655 (cache), 2.059 (aim off), 2.618 (ẩn menu), 2.658 (camera invalid), 3.572/3.579 (aim). Vì vậy phải thành free function, không thể là lambda private trong cache.
5. **`releaseTriggerIfDown` được gọi ngoài aim.** 2.066 (trig off), 2.139 (game closed), 2.616 (menu ẩn), 3.961 (trong aim), 4.066 (exit). Sau S3.3a là method `AimCtx`.
6. **`restoreViewportIfNeeded` được gọi ngoài aim.** 2.140, 2.617, 4.001, 4.067. Move thành method `AimCtx`, giữ nguyên thứ tự call.
7. **`dbgEnts` được main đọc.** MenuCtx 2.957 và diag 2.997. Có thể owned trong `AimCtx` và main đọc `aimCtx.dbgEnts`.
8. **`cam`, `camAddr`, `haveWrite` do main cập nhật mỗi frame.** Phải là ref.
9. **`AimTarget` là local trong aim block**, không rò ra ngoài; giữ local trong `tick`.
10. **`g_snap*` là file-scope static** ở 807–813, writer thread + aim cùng dùng. S3.1 export qua header; S3.3b move definition sang `ovl_aim.cpp`.
11. **`velextAlphaFromDt` đã non-static** (dùng bởi `tab_combat.cpp`), giữ declaration ở `ovl_state.h`.
12. **`loadHitboxProfile`/`hitboxRestoreAll` đang static** (607/693), cache gọi; chỉ cần bỏ `static` + prototype, không move.
13. **`worldRestoreAll`/`diagRate` đã export** từ S2; dùng lại.
14. **Không có `saveCfg`**; S3 không đụng cfg.

### 2.3 Vì sao thứ tự S3.1 → S3.2 → S3.3a

```
S3.1 cache ──► ents ──► S3.2 ESP (đọc ents, vm)
   │                         │
   └── stickPlayer/uisAddr ──┴──► S3.3a aim (đọc ents, vm, cam, camAddr, haveWrite)
```

- Cache là dependency duy nhất của cả hai; làm trước để ESP/aim không phải sửa lại lần 2.
- ESP không ghi state, nên gate in-game chỉ là visual; nó xác nhận cache trước khi aim dùng.
- Aim move sau cùng để tránh phải sửa call site 2 lần.

---

## 3. Kiến trúc đích

### 3.1 Cây file sau S3 core

```
src\
  overlay.cpp                 ~2.730  menu, main loop, input/hotkey, player mods,
                                      teleport/fly, hitbox, diag, camera, writer call
  ovl\
    ovl_view.h                ~35     screenW2S inline (body của w2s)
    ovl_cache.h               ~40     EntityCtx
    ovl_cache.cpp             ~330    cache 3 tầng verbatim
    ovl_esp.h                 ~35     EspCtx
    ovl_esp.cpp               ~245    ESP + radar verbatim
    ovl_aim.h                 ~150    AimSnap, MouseCtlState, VelextSens, constants,
                                      AimCtx, helper prototypes
    ovl_aim.cpp               ~800    tick + trigger + autoclick + restore viewport
    ovl_core.h                +2      qpcSeconds/qpcNs
    ovl_state.h               +2      loadHitboxProfile/hitboxRestoreAll
```

Sau S3.3b, `ovl_aim.cpp` ~1.450 dòng; `overlay.cpp` ~2.130.

### 3.2 Ai sở hữu state

| State | Owner sau S3 | Lý do |
|---|---|---|
| `ents`, `uisAddr`, `stickPlayer` | main | cache/aim/main cùng đọc; `uisAddr` cho chat guard |
| `cam`, `camAddr`, `haveWrite` | main | main refresh camera/write mỗi frame |
| `vpx/vpy/vpw/vph`, `sw/sh` | main | viewport/window; ESP + aim ref |
| `aimOn`, `trigOn`, `acOn` | main | hotkey toggle; tab đọc qua MenuCtx |
| `ecache/classCache/cDm/cPlayers/lastPlaceSeen/npcScanSkip` | `EntityCtx` | chỉ cache dùng |
| `lastAimQpc/lastActTick/lock*/trig*/ac*/acc*/ema*/mouseCtl/bp*/vpShifted/shiftedNow/dbgEnts/mouseSvc*` | `AimCtx` | chỉ aim dùng (trừ `dbgEnts` main đọc) |
| `g_snap/g_snapMu/g_snapEpoch/g_snapSeq` | `ovl_aim` (extern) | cache invalidate + aim publish + writer |
| `g_writer*`, `g_writeRateHz`, `g_currentCam` | `ovl_aim` (S3.3b) | writer private; main qua API |

### 3.3 Include graph (không vòng)

```
ovl_types.h ──► sdk.h ──► memory.h ──► windows.h
    ▲
    ├── ovl_cache.h
    ├── ovl_esp.h
    ├── ovl_aim.h ──► <atomic>, <mutex>
    └── ovl_view.h

ovl_cache.cpp  ──► ovl_cache.h, ovl_aim.h, ovl_core.h, ovl_cfg.h, ovl_state.h, ovl_world.h
ovl_esp.cpp    ──► ovl_esp.h, ovl_view.h, ovl_state.h, imgui.h
ovl_aim.cpp    ──► ovl_aim.h, ovl_view.h, ovl_core.h, ovl_cfg.h, ovl_state.h, imgui.h
overlay.cpp    ──► ovl_aim.h, ovl_cache.h, ovl_esp.h, ovl_view.h (+ các header S1/S2)
```

- `ovl_aim.h` **không** include `ovl_state.h`/`ovl_cfg.h` (tránh vòng; cfg extern chỉ cần trong .cpp).
- `ovl_cache.h`/`ovl_esp.h` chỉ include `ovl_types.h`.
- `ImDrawList` forward-declare trong header; `imgui.h` chỉ trong .cpp.
- Include bắt buộc trong .cpp:
  - `ovl_cache.cpp`: `<cmath>`, `<map>`, `<set>`, `<string>`, `<vector>`.
  - `ovl_esp.cpp`: `<cmath>`, `<cstdio>`, `<string>`, `imgui.h`.
  - `ovl_aim.cpp`: `<cmath>`, `<cstdio>`, `<thread>` (S3.3b), `imgui.h`.

---

## 4. Hợp đồng header

### 4.1 `src\ovl\ovl_view.h` (mới ở S3.2)

```cpp
#pragma once
#include "ovl_types.h"

inline bool screenW2S(const Vec3& w, const float* mt, int& sx, int& sy,
                      int vpx, int vpy, int vpw, int vph, int sw, int sh) {
    // Body y nguyên w2s cũ (1.888–1.900):
    int vw = vpw ? vpw : sw, vh = vph ? vph : sh;
    int ox = vpw ? vpx : 0, oy = vph ? vpy : 0;
    float x = mt[0] * w.x + mt[1] * w.y + mt[2] * w.z + mt[3];
    float y = mt[4] * w.x + mt[5] * w.y + mt[6] * w.z + mt[7];
    float ww = mt[12] * w.x + mt[13] * w.y + mt[14] * w.z + mt[15];
    if (ww < 0.1f) return false;
    sx = ox + (int)((vw / 2) * (1.0f + x / ww));
    sy = oy + (int)((vh / 2) * (1.0f - y / ww));
    return true;
}
```

### 4.2 `src\ovl\ovl_cache.h` (S3.1)

```cpp
#pragma once
#include "ovl_types.h"
#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct EntityCtx {
    EntityCtx(Memory& m_, SDK& sdk_, const Offsets& o_,
              std::vector<Ent>& ents_, uint64_t& uisAddr_,
              uint64_t& stickPlayer_)
      : m(m_), sdk(sdk_), o(o_), ents(ents_), uisAddr(uisAddr_),
        stickPlayer(stickPlayer_) {}

    Memory& m;
    SDK& sdk;
    const Offsets& o;
    std::vector<Ent>& ents;
    uint64_t& uisAddr;
    uint64_t& stickPlayer;

    std::map<uint64_t, Ent> ecache;
    std::map<uint64_t, std::string> classCache;
    uint64_t cDm = 0, cPlayers = 0, lastPlaceSeen = 0;
    size_t npcScanSkip = 0;

    void refreshEnts();

private:
    std::string classOf(uint64_t obj);
    void resolveParts(Ent& e, uint64_t model);
    void clearResolvedParts(Ent& e);
};
```

### 4.3 `src\ovl\ovl_esp.h` (S3.2)

```cpp
#pragma once
#include "ovl_types.h"

struct ImDrawList;

struct EspCtx {
    EspCtx(Memory& m_, const Offsets& o_, std::vector<Ent>& ents_,
           int& vpx_, int& vpy_, int& vpw_, int& vph_,
           int& sw_, int& sh_, uint64_t& camAddr_)
      : m(m_), o(o_), ents(ents_), vpx(vpx_), vpy(vpy_), vpw(vpw_),
        vph(vph_), sw(sw_), sh(sh_), camAddr(camAddr_) {}

    Memory& m;
    const Offsets& o;
    std::vector<Ent>& ents;
    int& vpx;
    int& vpy;
    int& vpw;
    int& vph;
    int& sw;
    int& sh;
    uint64_t& camAddr;

    void draw(ImDrawList* dl, const float* vm);

private:
    bool w2s(const Vec3& w, const float* mt, int& sx, int& sy);
};
```

### 4.4 `src\ovl\ovl_aim.h`

**S3.1 chỉ có phần đầu** (AimSnap + externs + invalidate). **S3.3a thêm** MouseCtlState/VelextSens/constants/helpers/AimCtx. **S3.3b thêm** writer API + selftest.

```cpp
#pragma once
#include "ovl_types.h"
#include <atomic>
#include <mutex>
#include <vector>

struct ImDrawList;

// ---- snapshot (move từ overlay.cpp 797–806) ----
struct AimSnap {
    bool ready = false;
    uint64_t epoch = 0;
    uint64_t seq = 0;
    Vec3 tgt{};
    Vec3 vel{};
    Vec3 cm{};
    uint64_t camAddr = 0;
    double qpc = 0;
};
extern std::mutex g_snapMu;
extern AimSnap g_snap;
extern std::atomic<uint64_t> g_snapEpoch;
extern std::atomic<uint64_t> g_snapSeq;
void invalidateAimSnapshot();

// ---- Phase C types ----
struct MouseCtlState {
    bool active = false;
    float abVX = 0.0f, abVY = 0.0f;
    float iX = 0.0f, iY = 0.0f;
    float idleT = 0.0f;
    bool settled = false;
    float lastEmaX = 0.0f, lastEmaY = 0.0f;
    bool lastEmaInit = false;
};
struct VelextSens {
    bool ok = false;
    float out1 = 1.0f;
    float out2 = 1.0f;
};

// Block hằng số 179–212 move nguyên giá trị; đổi static const -> inline constexpr.
inline constexpr float kVelextTauPerUnit = 0.008f;
inline constexpr float kVelextDtFallback = 0.004f;
inline constexpr float kVelextDtMax = 0.1f;
inline constexpr uint64_t kVelextGateNs = 4000000ULL;
inline constexpr float kVelextAlphaEps = 0.002f;
inline constexpr float kVelextDotDeadzone = 0.99996f;
inline constexpr float kVelextMouseDzPx = 1.5f;
inline constexpr float kVelextStepCapPxS = 14000.0f;
inline constexpr float kVelextStepCapLo = 12.0f;
inline constexpr float kVelextStepCapHi = 400.0f;
inline constexpr float kVelextBpMoveDeg = 0.02f;
inline constexpr uint64_t kVelextBpTimeoutNs = 60000000ULL;
inline constexpr uint64_t kVelextSensRvaA = 0x8D857D8ULL;
inline constexpr uint64_t kVelextSensRvaB = 0x8D857E0ULL;
inline constexpr uint64_t kVelextSensRvaC = 0x8D85820ULL;
inline constexpr uint64_t kVelextMouseSvcSensRva = 0x21C;
inline constexpr float kVelextSensInLo = 1e-3f;
inline constexpr float kVelextSensInHi = 100.0f;
inline constexpr float kVelextSensOutLo = 1e-4f;
inline constexpr float kVelextSensOutHi = 1000.0f;
inline constexpr float kVelextSensXFac = 0.5f;
inline constexpr float kVelextSensYFac = 0.385f;
inline constexpr float kVelextMinCosP = 0.15f;
inline constexpr float kDeg2Rad = 0.0174532925f;
inline constexpr float kRad2Deg = 57.2957795f;
inline constexpr int kVelextBpStaleMax = 6;
inline constexpr uint64_t kVelextBpProbeNs = 750000000ULL;

// Helper definitions ở overlay.cpp đến hết S3.3a; move sang ovl_aim.cpp ở S3.3b.
bool velextActGate(uint64_t nowNs, uint64_t* lastNsOut, float* dtOut,
                   float* alphaOut, float smoothing);
bool camYawPitchDeg(const float* m9, float* yawDeg, float* pitchDeg);
VelextSens readVelextSens(Memory& mem, uint64_t base, uint64_t msvc);
void velextMouseCounts(float eX, float eY, float focal, float p,
                       float out1, float out2, float alpha, float dt,
                       float* cxOut, float* cyOut);
bool mouseCursorHidden();
float wrap180f(float d);
void sendRelMouse(int mx, int my);

struct AimCtx {
    AimCtx(Memory& m_, SDK& sdk_, const Offsets& o_, std::vector<Ent>& ents_,
           bool& haveWrite_, uint64_t& camAddr_, Vec3& cam_,
           int& vpx_, int& vpy_, int& vpw_, int& vph_, int& sw_, int& sh_,
           bool& aimOn_, bool& trigOn_, bool& acOn_, uint64_t& stickPlayer_);

    // ---- refs (17) ----
    Memory& m;
    SDK& sdk;
    const Offsets& o;
    std::vector<Ent>& ents;
    bool& haveWrite;
    uint64_t& camAddr;
    Vec3& cam;
    int& vpx;
    int& vpy;
    int& vpw;
    int& vph;
    int& sw;
    int& sh;
    bool& aimOn;
    bool& trigOn;
    bool& acOn;
    uint64_t& stickPlayer;

    // ---- owned state (32; tên y hệt local cũ) ----
    int dbgEnts = 0;
    double lastAimQpc = 0;
    DWORD lastActTick = 0;
    Vec3 lockPos{};
    DWORD lockTick = 0;
    DWORD stickStartTick = 0;
    int trigPhase = 0;
    DWORD trigT0 = 0, lastFireTick = 0;
    int acPhase = 0;
    DWORD acT0 = 0, acNext = 0, acPressT0 = 0;
    float accX = 0, accY = 0;
    float emaTargetX = 0, emaTargetY = 0;
    uint64_t emaPrim = 0;
    bool emaInit = false;
    MouseCtlState mouseCtl;
    uint64_t mouseSvc = 0;
    DWORD mouseSvcRetry = 0;
    bool bpInFlight = false;
    uint64_t bpStampNs = 0;
    float bpYawDeg = 0, bpPitchDeg = 0;
    bool bpCursorHold = false;
    int bpStaleCount = 0;
    bool bpStaleHold = false;
    uint64_t bpProbeNs = 0;
    bool vpShifted = false;
    bool shiftedNow = false;

    void tick(ImDrawList* dl, const float* vm, bool haveVm,
              DWORD fgPid, bool typingNow);
    void autoclickTick(DWORD fgPid, bool typingNow);
    void releaseTriggerIfDown();
    void restoreViewportIfNeeded(bool force);
    void onAimDisabled();
    void onAutoclickEnabled();

private:
    bool w2s(const Vec3& w, const float* mt, int& sx, int& sy);
};

// S3.3b:
void aimWriterStart(Memory* pm, int rotOff);
void aimWriterStop();
void aimWriterSetRate(int hz);
void aimWriterSetCam(uint64_t camAddr);
int runAimSelfTest();
```

### 4.5 `src\ovl\ovl_core.h` (thêm ở S3.3a)

```cpp
double qpcSeconds();
uint64_t qpcNs();
```

`qpcSeconds`/`qpcNs` vẫn định nghĩa ở `overlay.cpp` (dùng bởi main loop, calibration, writer); chỉ bỏ `static`.

### 4.6 `src\ovl\ovl_state.h` (thêm ở S3.1)

```cpp
// overlay.cpp cung cấp cho cache (hitbox block chưa move ở S3).
void loadHitboxProfile(uint64_t place);
void hitboxRestoreAll(Memory& m, const Offsets& o);
```

`velextAlphaFromDt` giữ nguyên declaration hiện có.

---

## 5. Bảng capture chi tiết

### 5.1 `EntityCtx`

| Member | Kind | Nguồn | Ghi chú |
|---|---|---|---|
| `m` | `Memory&` | main | read class/name/position |
| `sdk` | `SDK&` | main | dataModel/placeId/walkChildren |
| `o` | `const Offsets&` | main | offset read |
| `ents` | `std::vector<Ent>&` | main 1.586 | output cho ESP/aim/srv |
| `uisAddr` | `uint64_t&` | main 1.589 | **main chat guard 1.944 đọc** |
| `stickPlayer` | `uint64_t&` | main 1.525 | cache clear khi GameLoaded=0 |
| `ecache` | owned `std::map<uint64_t,Ent>` | 1.584 | chỉ cache |
| `classCache` | owned `std::map<uint64_t,std::string>` | 1.585 | chỉ cache |
| `cDm`, `cPlayers`, `lastPlaceSeen` | owned `uint64_t` | 1.587 | chỉ cache |
| `npcScanSkip` | owned `size_t` | 1.588 | chỉ cache |
| `classOf`, `resolveParts`, `clearResolvedParts` | private methods | 1.590/1.601/1.648 | body verbatim, bỏ `auto ... = [&]` |
| `refreshEnts` | public method | 1.662 | body verbatim |

Call site: `entCtx.refreshEnts();` thay `refreshEnts();` ở 2.151.

Giữ ở main: `std::vector<Ent> ents;`, `uint64_t uisAddr = 0;`.
De-static: `loadHitboxProfile` (607), `hitboxRestoreAll` (693).
Free functions dùng trong body: `worldRestoreAll`, `invalidateAimSnapshot`, `diagRate`, `cfgAimNPCs`.

### 5.2 `EspCtx`

| Member | Kind | Nguồn | Ghi chú |
|---|---|---|---|
| `m` | `Memory&` | main | read position/health/camera |
| `o` | `const Offsets&` | main | offsets |
| `ents` | `std::vector<Ent>&` | main | danh sách entity |
| `vpx/vpy/vpw/vph` | `int&` | main 1.571 | viewport game client |
| `sw/sh` | `int&` | main 1.409 | screen fallback |
| `camAddr` | `uint64_t&` | main 1.513 | radar rotation |
| `draw(dl, vm)` | method | 3.060–3.283 | body verbatim |
| `w2s` | private method | 1.888–1.900 | forward `screenW2S` |

Không owned state. `dl`/`vm` là param. `cfgEsp*`/`cfgRadar*`/`cfgHudOn` là global.

### 5.3 `AimCtx` — 17 ref

| # | Member | Kind | Nguồn | Vì sao ref |
|---:|---|---|---|---|
| 1 | `m` | `Memory&` | main | read/write |
| 2 | `sdk` | `SDK&` | main | dataModel/walkChildren/base |
| 3 | `o` | `const Offsets&` | main | offsets |
| 4 | `ents` | `std::vector<Ent>&` | main | target list |
| 5 | `haveWrite` | `bool&` | main 1.375 | mode 1/2 write gate |
| 6 | `camAddr` | `uint64_t&` | main 1.513 | camera |
| 7 | `cam` | `Vec3&` | main 1.512 | distance filter |
| 8 | `vpx` | `int&` | main 1.571 | crosshair x |
| 9 | `vpy` | `int&` | main 1.571 | crosshair y |
| 10 | `vpw` | `int&` | main 1.571 | viewport w |
| 11 | `vph` | `int&` | main 1.571 | viewport h/focal |
| 12 | `sw` | `int&` | main 1.409 | fallback w |
| 13 | `sh` | `int&` | main 1.409 | fallback h |
| 14 | `aimOn` | `bool&` | main 1.522 | hotkey state |
| 15 | `trigOn` | `bool&` | main 1.530 | hotkey state |
| 16 | `acOn` | `bool&` | main 1.531 | hotkey + burst end ghi false |
| 17 | `stickPlayer` | `uint64_t&` | main 1.525 | cache + key release cùng dùng |

### 5.4 `AimCtx` — 32 owned state

| Nhóm | Members | Nguồn | Ghi chú |
|---|---|---|---|
| Sticky/lock | `lockPos`, `lockTick`, `stickStartTick` | 1.528–1.529, 1.557 | reset trong `onAimDisabled` |
| Timing | `lastAimQpc`, `lastActTick` | 1.526–1.527 | reset trong `onAimDisabled` |
| Trigger | `trigPhase`, `trigT0`, `lastFireTick` | 1.534–1.535 | `releaseTriggerIfDown` |
| Autoclick | `acPhase`, `acT0`, `acNext`, `acPressT0` | 1.532–1.533 | `onAutoclickEnabled` |
| Mouse carry | `accX`, `accY` | 1.536 | reset trong `onAimDisabled` |
| EMA | `emaTargetX`, `emaTargetY`, `emaPrim`, `emaInit` | 1.544–1.546 | reset trong `onAimDisabled` |
| Mouse control | `mouseCtl` (`MouseCtlState`) | 1.547–1.556 | type move sang header |
| MouseService | `mouseSvc`, `mouseSvcRetry` | 1.514–1.515 | tick tự reset khi `!wantAim` |
| Back-pressure | `bpInFlight`, `bpStampNs`, `bpYawDeg`, `bpPitchDeg` | 1.537–1.539 | tick reset khi `!wantAim` |
| Back-pressure guard | `bpCursorHold`, `bpStaleCount`, `bpStaleHold`, `bpProbeNs` | 1.540–1.543 | tick reset khi `!wantAim` |
| Viewport shift | `vpShifted`, `shiftedNow` | 1.558–1.559 | `restoreViewportIfNeeded` method |
| Diag | `dbgEnts` | 1.565 | main đọc `aimCtx.dbgEnts` |

### 5.5 `AimCtx` — parameter / free

| Item | Xử lý | Nguồn |
|---|---|---|
| `haveVm` | param `bool` | 3.048–3.057; call chỉ trong `if (haveVm)` |
| `dl` | param `ImDrawList*` | 3.058 |
| `vm` | param `const float*` | 3.046–3.049 |
| `fgPid` | param `DWORD` | 2.157 |
| `typingNow` | param `bool` | 1.943 |
| `invalidateAimSnapshot` | free function | 1.655; gọi 2.059/2.618/2.658/3.572/3.579 |
| `g_snap*` | extern globals | 807–813 |
| `qpcSeconds/qpcNs` | free functions | 146/214 |
| `velextActGate`, `camYawPitchDeg`, `readVelextSens`, `velextMouseCounts`, `mouseCursorHidden`, `wrap180f`, `sendRelMouse` | free functions | 154–362 |
| `cfgAim*`, `cfgTrig*`, `cfgAC*` | globals | `ovl_state.h` + `ovl_cfg.h` |
| `AimTarget` | local trong `tick` | 3.334–3.339 |

### 5.6 `onAimDisabled` — map chính xác từ block 2.045–2.059

| Dòng cũ | Dòng mới |
|---|---|
| `stickPlayer = 0;` (2.046) | giữ ở main trước call |
| `stickStartTick = 0;` | `stickStartTick = 0;` |
| `lockTick = 0;` | `lockTick = 0;` |
| `lockPos = Vec3{};` | `lockPos = Vec3{};` |
| `lastAimQpc = 0;` | `lastAimQpc = 0;` |
| `lastActTick = 0;` | `lastActTick = 0;` |
| `accX = 0.0f;` | `accX = 0.0f;` |
| `accY = 0.0f;` | `accY = 0.0f;` |
| `emaTargetX = 0.0f;` | `emaTargetX = 0.0f;` |
| `emaTargetY = 0.0f;` | `emaTargetY = 0.0f;` |
| `emaPrim = 0;` | `emaPrim = 0;` |
| `emaInit = false;` | `emaInit = false;` |
| `mouseCtl = MouseCtlState{};` | `mouseCtl = MouseCtlState{};` |
| `invalidateAimSnapshot();` (2.059) | giữ ở main sau call |

Main sau S3.3a:

```cpp
if (!aimOn) {
    stickPlayer = 0;
    aimCtx.onAimDisabled();
    invalidateAimSnapshot();
}
```

`onAimDisabled` **không** reset `mouseSvc/mouseSvcRetry/bp*` — đúng như code cũ (chúng reset trong tick khi `!wantAim`).

### 5.7 Constructor order + assert tạm

Thứ tự member/constructor (phải khớp):

```
m, sdk, o, ents, haveWrite, camAddr, cam, vpx, vpy, vpw, vph, sw, sh,
aimOn, trigOn, acOn, stickPlayer
```

Constructor definition liệt kê từng cặp `member(member_)` trên cùng dòng để soi swap. Sau construct, thêm assert tạm (gỡ ở S3.4):

```cpp
assert(&aimCtx.vpx == &vpx); assert(&aimCtx.vpy == &vpy);
assert(&aimCtx.vpw == &vpw); assert(&aimCtx.vph == &vph);
assert(&aimCtx.sw == &sw);   assert(&aimCtx.sh == &sh);
assert(&aimCtx.aimOn == &aimOn); assert(&aimCtx.trigOn == &trigOn);
assert(&aimCtx.acOn == &acOn);
assert(&espCtx.vpx == &vpx); assert(&espCtx.vpy == &vpy);
assert(&espCtx.vpw == &vpw); assert(&espCtx.vph == &vph);
assert(&espCtx.sw == &sw);   assert(&espCtx.sh == &sh);
assert(&entCtx.uisAddr == &uisAddr);
assert(&entCtx.stickPlayer == &stickPlayer);
```

### 5.8 Không move

- `frame` (1.516) — giữ main; viewport refresh 3.037–3.045 giữ main.
- `haveVm` (3.048) — tính ở main; param.
- `dl` (3.058) — main tạo; param.
- `vm[16]`, `ve` — main; param `vm`.
- `fgPid`, `typingNow` — main; param.
- `calibPhase/calibT0/calibYaw0/calibCounts` — calibration, không phải aim.
- `tpMsg/tpMsgTick`, `toggleName/aimName`, `place`, `visible`, `done`, `loopMs`.
- Player mods, teleport/fly, hitbox, world, server, menu.
- `AimTarget` — local trong tick.

---

## 6. Các bước triển khai

### S3.0 — Snapshot + baseline (~30 phút)

1. Tạo `backup\S3PRE_20261020\`:
   - `src\overlay.cpp`, toàn bộ `src\ovl\*.h/*.cpp`, `build_fast.bat`, `build.bat`, `ext_overlay.exe`, `overlay.cfg`.
   - `sha256.txt` cho các file trên.
2. Chạy baseline, lưu vào snapshot:
   - `build_fast.bat` → exit 0, 0 warning.
   - `build.bat` → exit 0, 0 error, 0 tool fail.
   - `ext_overlay.exe -aimtest` → exit 0, 0 `FAIL`; lưu **raw stdout** (dùng `cmd /c "... > aimtest_stdout.txt 2>&1"`, không dùng `>` của PowerShell vì UTF-16). Ghi 522 dòng / 0 FAIL.
   - `ext_overlay.exe -srvtest 0` → parser PASS; network list FAIL là WARN.
   - 8 tab hits → `backup\s3_baseline_hits.txt`; so `backup\s2_hits_final.txt`.
3. Xác nhận `src\overlay.cpp` hash `5DAA…`, CRLF=0, BOM=false.

**Gate:** baseline khớp S2POST; không sửa source.

**Rollback:** không cần (chưa sửa).

---

### S3.1 — Entity cache (~2–2,5 h)

**File mới:** `src\ovl\ovl_cache.h`, `src\ovl\ovl_cache.cpp`, `src\ovl\ovl_aim.h` (scaffolding).

**Việc làm:**

1. Tạo `ovl_aim.h` với `AimSnap`, extern `g_snapMu/g_snap/g_snapEpoch/g_snapSeq`, `invalidateAimSnapshot()`.
2. Trong `overlay.cpp`:
   - Include `ovl/ovl_aim.h`.
   - Xóa `struct AimSnap` 797–806 (đã ở header).
   - Bỏ `static` ở 4 định nghĩa `g_snapMu/g_snap/g_snapEpoch/g_snapSeq`.
   - Hoist lambda `invalidateAimSnapshot` 1.655–1.661 thành free function đặt cạnh các global snapshot; body y nguyên.
   - Giữ `std::vector<Ent> ents;` và `uint64_t uisAddr = 0;` ở main.
   - Xóa block 1.582–1.887; thay bằng:
     ```cpp
     EntityCtx entCtx{ m, sdk, o, ents, uisAddr, stickPlayer };
     ```
     đặt sau chỗ khai báo `ents`/`uisAddr`.
   - `refreshEnts();` ở 2.151 → `entCtx.refreshEnts();`.
3. `ovl_cache.cpp`: move 4 lambda thành method, body verbatim; chỉ đổi dòng khai báo:
   - `auto classOf = [&](uint64_t obj) -> std::string {` → `std::string EntityCtx::classOf(uint64_t obj) {`
   - `auto resolveParts = [&](Ent& e, uint64_t model) {` → `void EntityCtx::resolveParts(Ent& e, uint64_t model) {`
   - `auto clearResolvedParts = [](Ent& e) {` → `void EntityCtx::clearResolvedParts(Ent& e) {`
   - `auto refreshEnts = [&]() {` → `void EntityCtx::refreshEnts() {`
4. Bỏ `static` ở `loadHitboxProfile` (607), `hitboxRestoreAll` (693); thêm 2 prototype vào `ovl_state.h`.
5. Thêm `src\ovl\ovl_cache.cpp` vào **cả** `build_fast.bat` (dòng 11) và `build.bat` (dòng 26), ngay sau `src\ovl\ovl_world.cpp`.

**Gate S3.1:**

- `build_fast.bat`: exit 0, 0 warning/error.
- `build.bat`: exit 0, 0 error, 0 tool fail.
- `-aimtest`: exit 0, 0 FAIL; stdout byte-identical S3.0.
- `-srvtest 0`: parser PASS.
- 8 tab hits khớp S2 final (tab3 first-run có thể 16, chạy lại 17).
- In-game cache smoke (§9.1).
- `aim_diag.log`: `entity-refresh` có `cache/ents` hợp lý; không có `entity-reresolve` loop.
- Snapshot `backup\S3.1_cache\` (overlay.cpp, ovl_cache.*, ovl_aim.h, ovl_state.h, 2 build script, exe).

**Rollback:** copy `backup\S3PRE_20261020\src\overlay.cpp`, `src\ovl\ovl_state.h`, 2 build script; xóa `ovl_cache.*`, `ovl_aim.h`; build lại.

---

### S3.2 — ESP + radar (~1,5 h)

**File mới:** `src\ovl\ovl_view.h`, `src\ovl\ovl_esp.h`, `src\ovl\ovl_esp.cpp`.

**Việc làm:**

1. `ovl_view.h`: move body `w2s` 1.888–1.900 thành `screenW2S` inline (param hóa `vpx/vpy/vpw/vph/sw/sh`).
2. `ovl_esp.h/.cpp`: move 3.060–3.283 vào `EspCtx::draw(ImDrawList* dl, const float* vm)`; body verbatim. `w2s` private method forward `screenW2S`.
3. `overlay.cpp`:
   - Include `ovl/ovl_view.h`, `ovl/ovl_esp.h`.
   - Construct `EspCtx espCtx{ m, o, ents, vpx, vpy, vpw, vph, sw, sh, camAddr };` sau khi có `ents`.
   - Trong `if (haveVm)`:
     ```cpp
     espCtx.draw(dl, vm);
     // aim block vẫn nằm sau (S3.2 chưa move)
     ```
   - Thay body `w2s` lambda bằng forwarder (aim còn dùng):
     ```cpp
     auto w2s = [&](const Vec3& w, const float* mt, int& sx, int& sy) {
         return screenW2S(w, mt, sx, sy, vpx, vpy, vpw, vph, sw, sh);
     };
     ```
4. Thêm `src\ovl\ovl_esp.cpp` vào cả 2 build script, sau `ovl_cache.cpp`.

**Gate S3.2:**

- Build fast/full 0/0.
- `-aimtest` byte-identical; `-srvtest 0` PASS.
- 8 tab hits khớp.
- In-game ESP checklist (§9.2): box/name/HP/skeleton/head/line/fill/OOF/radar; team/NPC filter; max dist; resize/move game window.
- Snapshot `backup\S3.2_esp\`.

**Rollback:** copy snapshot S3.1 + xóa `ovl_esp.*`, `ovl_view.h`; build lại.

---

### S3.3a — Aim tick + trigger + autoclick (~3–3,5 h)

**File mới:** `src\ovl\ovl_aim.cpp`; mở rộng `ovl_aim.h`.

**Việc làm:**

1. `ovl_aim.h`: thêm `MouseCtlState`, `VelextSens`, block hằng số (179–212), helper prototypes, `AimCtx` (17 ref + 32 owned + methods).
2. `ovl_aim.cpp`:
   - `AimCtx` constructor (17 ref, init list đúng thứ tự).
   - `tick`: move 3.285–3.999; body verbatim; `releaseTriggerIfDown()` giữ nguyên text (member).
   - `autoclickTick`: move 4.008–4.050.
   - `releaseTriggerIfDown`: move 1.901–1.909.
   - `restoreViewportIfNeeded`: move 1.910–1.926.
   - `onAimDisabled`: theo bảng §5.6.
   - `onAutoclickEnabled`: `acT0 = GetTickCount(); acNext = acT0;`.
   - `invalidateAimSnapshot`: move free function từ `overlay.cpp` sang đây (S3.1 đã hoist).
   - `w2s` forwarder.
3. `overlay.cpp`:
   - Include `ovl/ovl_aim.h`.
   - Xóa các khai báo state 1.525–1.559 **trừ** `stickPlayer`, `aimOn`, `trigOn`, `acOn`; xóa thêm `mouseSvc`/`mouseSvcRetry` (1.514–1.515), `dbgEnts` (1.565), và `MouseCtlState` definition + `mouseCtl`.
   - Construct:
     ```cpp
     AimCtx aimCtx{ m, sdk, o, ents, haveWrite, camAddr, cam,
                    vpx, vpy, vpw, vph, sw, sh,
                    aimOn, trigOn, acOn, stickPlayer };
     ```
   - Reset block 2.045–2.059 → `stickPlayer = 0; aimCtx.onAimDisabled(); invalidateAimSnapshot();`.
   - Trig toggle 2.066 → `aimCtx.releaseTriggerIfDown();`.
   - AC toggle 2.070–2.071 và 2.078–2.079 → `aimCtx.onAutoclickEnabled();`.
   - `releaseTriggerIfDown()` ở 2.139/2.616/4.066 → `aimCtx.releaseTriggerIfDown();`.
   - `restoreViewportIfNeeded(...)` ở 2.140/2.617/4.001/4.067 → `aimCtx.restoreViewportIfNeeded(...);`.
   - Xóa `restoreViewportIfNeeded` lambda 1.910–1.926.
   - Xóa aim block 3.285–3.999; trong `if (haveVm)` gọi:
     ```cpp
     aimCtx.tick(dl, vm, true, fgPid, typingNow);
     ```
     **Giữ call bên trong `if (haveVm)`**; truyền `true` để thân hàm giữ nguyên điều kiện `&& haveVm`.
   - Xóa autoclick block 4.008–4.050; gọi `aimCtx.autoclickTick(fgPid, typingNow);` đúng vị trí cũ.
   - `MenuCtx` 2.957 và diag 2.997: `dbgEnts` → `aimCtx.dbgEnts`.
   - Xóa `w2s` forwarder (không còn call site).
4. De-static các helper (định nghĩa vẫn ở `overlay.cpp`):
   `qpcSeconds`, `qpcNs`, `sendRelMouse`, `velextActGate`, `camYawPitchDeg`, `readVelextSens`, `velextMouseCounts`, `mouseCursorHidden`, `wrap180f`.
   Thêm prototype `qpc*` vào `ovl_core.h`, còn lại vào `ovl_aim.h`.
   Move block hằng số 179–212 vào `ovl_aim.h` (`inline constexpr`), xóa ở `overlay.cpp`.
5. Thêm `src\ovl\ovl_aim.cpp` vào cả 2 build script, sau `ovl_esp.cpp`.
6. Thêm assert tạm §5.7.

**Gate S3.3a:**

- Build fast/full 0/0.
- `-aimtest` byte-identical (helper/selftest chưa move; chỉ aim body move).
- 8 tab hits khớp.
- `assert` ref không fire.
- In-game aim checklist (§9.3): mode 0/1/2, smoothing, sticky, bone, prediction, FOV, deadzone, trigger 3 phase, autoclick burst/toggle, typing guard, menu ẩn/exit nhả LMB, viewport restore.
- Snapshot `backup\S3.3a_aim\`.

**Rollback:** copy snapshot S3.2 + xóa `ovl_aim.cpp`; build lại.

---

### S3.3b — Velext math + writer + `-aimtest` (optional, ~2,5 h)

**Mục tiêu:** `ovl_aim.cpp` sở hữu trọn aim math/writer; `overlay.cpp` chỉ còn call API.

**Việc làm:**

1. Move sang `ovl_aim.cpp`:
   - Quat helpers 60–140 (`Quat`, `matToQuat`, `slerpQ`, `quatToMat`).
   - Velext helper definitions 177–362 (constants đã ở header): `velextAlphaFromDt`, `velextActGate`, `wrap180f`, `camYawPitchDeg`, `velextSensValidIn`, `readVelextSens`, `velextMouseCounts`, `mouseCursorHidden`.
   - `g_velLastActNs` (219) đi cùng `velextActGate`.
   - `AimSnap` definitions `g_snapMu/g_snap/g_snapEpoch/g_snapSeq` (807–813).
   - `g_writerRun/g_writerCount/g_writeRateHz/g_currentCam` (809–814).
   - `buildAimMatrix` (907–962), `snapPos` (943), `writerThread` (963–1103).
   - `runAimSelfTest` (1.106–1.307).
2. API trong `ovl_aim.h`:
   ```cpp
   void aimWriterStart(Memory* pm, int rotOff);
   void aimWriterStop();
   void aimWriterSetRate(int hz);
   void aimWriterSetCam(uint64_t camAddr);
   int runAimSelfTest();
   ```
   `aimWriterStart` tạo `std::thread` nội bộ; `aimWriterStop` set `g_writerRun=false` + join.
3. `overlay.cpp`:
   - 1.434 `g_writeRateHz = cfgAimRate;` → `aimWriterSetRate(cfgAimRate);`
   - 1.438 `std::thread writerTh(...)` → `aimWriterStart(&m, o.cameraRotation);`
   - 2.638/2.652 `g_currentCam.store(...)` → `aimWriterSetCam(ch);` / `aimWriterSetCam(0);`
   - 4.079 `g_writerRun = false; ... join` → `aimWriterStop();`
   - 1.332 `runAimSelfTest()` giữ nguyên (declaration từ `ovl_aim.h`).
   - Xóa các definition đã move; giữ `qpcSeconds/qpcNs/sendRelMouse/wrapPi/lookYaw` ở `overlay.cpp` (calibration/main dùng).
4. Không đổi `diagOpen/diagClose/diagRate`, mouse-clip, `g_diag*`.

**Gate S3.3b:**

- Build fast/full 0/0.
- `-aimtest` **byte-identical** S3.0 (so file, không chỉ exit code).
- 8 tab hits khớp.
- In-game mode 1 writer: lock mượt, không race, không stuck; mode 0 vẫn chạy.
- `aim_diag.log`: `snapshot-publish`/`aim-bp`/`writer` không đổi hành vi.
- Snapshot `backup\S3.3b_aimmath\`.

**Rollback:** copy snapshot S3.3a; build lại.

---

### S3.4 — Verify cuối + docs + snapshot (~1,5 h)

1. Gỡ assert tạm §5.7.
2. Full gate: build_fast, build, `-aimtest` byte-identical, `-srvtest 0`, 8 tab hits, cfg key-set, LF/BOM.
3. Cập nhật `AGENTS.md`:
   - Module map thêm `ovl_cache`, `ovl_esp`, `ovl_aim`, `ovl_view`.
   - Build list đã có 3 file mới.
   - Gotcha: `AimSnap/g_snap*` extern trong `ovl_aim.h`; `invalidateAimSnapshot` free; `aimWriter*` API; `frame`/viewport refresh ở main; `uisAddr/stickPlayer` ref.
4. Viết `HANDOFF_S3_aim_esp_cache.md` theo mẫu S2: kết quả, file map, gate, sai lệch, checklist, rollback.
5. Snapshot `backup\S3POST_20261020\` (source + exe + cfg + sha256 + log).

**Gate S3.4:** tất cả §8 pass; `overlay.cpp` ≤ 2.800 dòng (core) hoặc ≤ 2.150 (full).

---

## 7. Build scripts — thay đổi chính xác

### `build_fast.bat` (dòng 11)

Trước:

```
... src\ovl\ovl_world.cpp src\ovl\tab_body.cpp ...
```

Sau S3.1:

```
... src\ovl\ovl_world.cpp src\ovl\ovl_cache.cpp src\ovl\tab_body.cpp ...
```

Sau S3.2:

```
... src\ovl\ovl_world.cpp src\ovl\ovl_cache.cpp src\ovl\ovl_esp.cpp src\ovl\tab_body.cpp ...
```

Sau S3.3a:

```
... src\ovl\ovl_world.cpp src\ovl\ovl_cache.cpp src\ovl\ovl_esp.cpp src\ovl\ovl_aim.cpp src\ovl\tab_body.cpp ...
```

### `build.bat` (dòng 26)

Thay đúng chuỗi tương tự trong lệnh overlay. **Thêm từng bước**, không thêm cả 3 file ở S3.1 (file chưa tồn tại → link fail).

### Không đổi

- Danh sách 25 tool standalone.
- `src\ntwrite.cpp` trong overlay.
- Libs: `-lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm`.

---

## 8. Verification protocol

### 8.1 Build

```bat
build_fast.bat
build.bat
```

- `build_fast`: exit 0, **0 warning**, 0 error; lưu `build_log_s3x_fast.txt`.
- `build.bat`: exit 0, **0 error**, **0 tool fail**; lưu `build_log_s3x_full.txt`.
- Không có `undefined reference` (đặc biệt `loadHitboxProfile`, `hitboxRestoreAll`, `invalidateAimSnapshot`, `velextActGate`, `qpcNs`, `aimWriter*`).

### 8.2 Self-test

```bat
cmd /c "ext_overlay.exe -aimtest > aimtest_s3x.txt 2>&1"
ext_overlay.exe -srvtest 0
```

- `-aimtest`: exit 0, 0 `FAIL`; **`fc /b`** với `backup\S3PRE_20261020\aimtest_stdout.txt`. Không chỉ so exit code.
- `-srvtest 0`: parser PASS; `[6] network list` FAIL khi offline là WARN, `geo` phụ thuộc mạng.

### 8.3 8 tab hits

Sandbox riêng cho từng tab:

1. Copy `ext_overlay.exe` + `offsets_cache.json` + `overlay.cfg` vào thư mục tạm.
2. Sửa cfg: `menu.tab=N`, `menu.show=1`, `menu.collapsed=0`, `menu.pos=928,173`.
3. Chạy `ext_overlay.exe -diag -nohide -t 4`.
4. Lưu dòng `menu tab=N ... hits=M ...`; exit 0; không còn process.

Baseline S2 final:

| Tab | Hits | Size |
|---|---:|---|
| 0 | 17 | 344,253 |
| 1 | 15 | 380,236 |
| 2 | 28 | 344,525 |
| 3 | 17 | 344,500 |
| 4 | 25 | 344,455 |
| 5 | 23 | 344,455 |
| 6 | 10 | 344,355 |
| 7 | 32 | 344,525 |

Lưu ý: tab3 first-run có thể `hits=16`; chạy lại 17. `menu.size` auto-write sau 20 frame — dùng cfg mới cho mỗi tab.

### 8.4 Cfg

- Key-set parser: **112 literal key**, không thêm/bớt (so trước/sau).
- Default template trong thư mục trống: `overlay.cfg` byte-identical với S3PRE (cùng thời điểm).
- Sentinel: `toggle=H, aim.key=Q, aim.keymode=0, aim.mode=2, aim.rate=777, aim.smoothing=13, aim.pid=1, menu.tab=7, menu.pos=200,150` → stdout/aim_diag đúng như S2.
- Không đổi tên/default/thứ tự tab.

### 8.5 Source hygiene

```powershell
# cho overlay.cpp + mọi file ovl/*
# kỳ vọng: crlf=0, bom=false
```

- `src\overlay.cpp` và `src\ovl\*` **LF-only, no BOM**.
- Không dùng editor tự CRLF; script Python ghi `newline="\n"`.

### 8.6 Process

- `Get-Process ext_overlay` → không còn sau `-t`.
- Không chạy `velext` cùng lúc.

---

## 9. Checklist in-game (user làm sau gate tự động)

### 9.1 S3.1 — cache

- [ ] Vào game, ESP hiện đủ player/NPC; tên/team/màu đúng.
- [ ] Respawn/stream-in: box cập nhật model mới, không còn box ma ở vị trí cũ.
- [ ] Map hop: cache clear, không còn entity map cũ; `aim_diag.log` có `entity-refresh` mới.
- [ ] NPC scan không nhân đôi box player (không còn orange box chồng red/green).
- [ ] Chat focus guard vẫn hoạt động (`uisAddr`): gõ chat không aim/không hotkey.
- [ ] Không hitch 0.5 s khi refresh; FPS ổn định.
- [ ] `entity-refresh` log: `cache`/`ents` tăng giảm hợp lý, không loop `entity-reresolve`.
- [ ] Thoát game/overlay: không crash, không process orphan.

### 9.2 S3.2 — ESP/radar

- [ ] Box, name, HP bar, head dot, skeleton, line, fill bật/tắt độc lập đúng.
- [ ] OOF arrow + distance đúng hướng/khoảng cách.
- [ ] Team/NPC filter: đổi màu và ẩn/hiện đúng.
- [ ] Max distance: entity xa biến mất, gần hiện lại.
- [ ] Radar: self ở giữa, camera-up = up, dot đúng vị trí; size/range đổi đúng.
- [ ] Move/resize game window: ESP vẫn khớp (viewport refresh mỗi frame).
- [ ] Menu ẩn/hiện: ESP không nhấp nháy, không vẽ đè menu.
- [ ] Không crash khi `ents=0`/`vm=0`.

### 9.3 S3.3a — aim/trigger/autoclick

- [ ] Aim mode 0 (velext mouse): khóa mượt, không giật; smoothing 1/5/13 đổi đúng.
- [ ] Aim mode 1 (memory writer): khóa đúng, không race, không lệch góc.
- [ ] Aim mode 2 (viewport shift): tâm bắn đúng, `restoreViewportIfNeeded` trả identity khi nhả/ẩn/thoát.
- [ ] Sticky: giữ target qua respawn (Player ID), drop khi quá xa/hết `stick.time`.
- [ ] Bone 0/2..5: head/arm/leg đúng; prediction bật/tắt đúng.
- [ ] FOV, deadzone, interval, max dist, ignore team đúng.
- [ ] Trigger: phase 0→1→2, delay/press/cooldown/pad đúng; nhả chuột khi mất target.
- [ ] Autoclick: burst duration, interval, press; toggle/held mode; hết burst tự tắt.
- [ ] Typing guard: gõ chat không aim/trigger/autoclick.
- [ ] Ẩn menu (toggle) và thoát overlay: LMB được nhả, viewport restore, không stuck input.
- [ ] Panic/END: thoát sạch, không process orphan.
- [ ] `aim_diag.log`: `snapshot-publish`, `aim-bp`, `aim-stale-hold`, `aim-cursor-hold` không đổi bất thường.

### 9.4 S3.3b — writer/math (nếu làm)

- [ ] Mode 1 khóa như S3.3a; không deadlock khi bật/tắt aim liên tục.
- [ ] `-aimtest` byte-identical.
- [ ] Map hop/camera invalid: snapshot invalidate đúng, không lock camera cũ.
- [ ] Thoát overlay: writer join sạch, không treo.

---

## 10. Rủi ro & cách chặn

| # | Rủi ro | Mức | Cách chặn |
|---:|---|---|---|
| 1 | Swap ref cùng kiểu trong `AimCtx` (6 int, 3 bool) | Cao | Assert địa chỉ §5.7; constructor init list từng cặp; review |
| 2 | Cache stale/ghost/NPC trùng | Cao | Move verbatim; in-game §9.1; `entity-refresh` log |
| 3 | Trigger stuck LMB | Cao | `releaseTriggerIfDown` ở hide/exit/trig-off; test panic |
| 4 | Viewport shift không restore | Cao | Move `restoreViewportIfNeeded` method; test mode 2 + exit |
| 5 | `frame` bị move/tăng | Trung bình | Giữ viewport refresh ở main; không sửa `frame` |
| 6 | Snapshot deadlock/epoch sai | Trung bình | Giữ nguyên lock order; mode 1 test; `invalidate` free function |
| 7 | Thiếu file trong build list | Cao | Thêm từng bước vào **cả** 2 script; full build mỗi bước |
| 8 | CRLF/BOM xâm nhập | Trung bình | Verify §8.5; script ghi LF |
| 9 | `uisAddr`/`stickPlayer` bị owned sai | Cao | Giữ ref; chat guard + respawn test |
| 10 | `-aimtest` đổi output | Trung bình | So `fc /b`, không chỉ exit code |
| 11 | ODR `g_snap` extern | Trung bình | Chỉ 1 definition; header chỉ `extern` |
| 12 | Build tool standalone fail | Thấp | Full build mỗi bước; overlay-only file không ảnh hưởng tool |

---

## 11. Rollback

| Snapshot | Nội dung |
|---|---|
| `backup\S3PRE_20261020\` | overlay.cpp, ovl/*, build scripts, exe, cfg, sha256, baseline logs |
| `backup\S3.1_cache\` | overlay.cpp, ovl_cache.*, ovl_aim.h, ovl_state.h, build scripts, exe |
| `backup\S3.2_esp\` | + ovl_esp.*, ovl_view.h |
| `backup\S3.3a_aim\` | + ovl_aim.*, ovl_core.h |
| `backup\S3.3b_aimmath\` | + ovl_aim.cpp (full), overlay.cpp |
| `backup\S3POST_20261020\` | toàn bộ source + exe + cfg + log sau S3 |

Khôi phục:

```bat
xcopy /E /I /Y backup\S3.1_cache\src src
copy /Y backup\S3.1_cache\build_fast.bat .
copy /Y backup\S3.1_cache\build.bat .
```

**Không dùng `git checkout src\overlay.cpp`** — autocrlf có thể đổi LF→CRLF. Rollback chính là snapshot `backup\`.

---

## 12. Ước lượng

| Bước | Nội dung | Thời gian |
|---|---|---:|
| S3.0 | snapshot + baseline | 0,5 h |
| S3.1 | cache + snapshot scaffolding | 2,0–2,5 h |
| S3.2 | ESP + radar | 1,5 h |
| S3.3a | aim tick + trigger + autoclick + restore | 3,0–3,5 h |
| S3.4 | verify + docs + snapshot | 1,5 h |
| **Core** | **S3.0–S3.4** | **8,5–9,5 h** |
| S3.3b | velext math + writer + selftest | 2,5 h |
| **Full** | **core + S3.3b** | **11–12 h** |

---

## 13. Definition of Done

- [ ] `src\overlay.cpp` ≤ **2.800 dòng** (core) hoặc ≤ **2.150 dòng** (full).
- [ ] `ovl_cache`, `ovl_esp`, `ovl_aim` sở hữu đúng khối; không còn body aim/ESP/cache trong `overlay.cpp`.
- [ ] Không đổi cfg key/default/thứ tự tab; key-set 112.
- [ ] Không đổi aim math/hằng số/draw order.
- [ ] `build_fast` + `build.bat` 0 warning/0 error/0 tool fail.
- [ ] `-aimtest` byte-identical; `-srvtest 0` PASS.
- [ ] 8 tab hits khớp baseline.
- [ ] In-game checklist §9 pass (cache/ESP/aim/trigger/autoclick/writer).
- [ ] LF-only, no BOM cho `overlay.cpp` + `ovl/*`.
- [ ] `AGENTS.md` + `HANDOFF_S3_aim_esp_cache.md` cập nhật.
- [ ] Snapshot `S3POST_20261020` + rollback snapshot từng bước.

---

## 14. Thứ tự thực thi (tóm tắt)

```
S3.0  snapshot + baseline
S3.1  ovl_aim.h scaffold + ovl_cache.* + hitbox de-static
S3.2  ovl_view.h + ovl_esp.*
S3.3a ovl_aim.* (tick/trigger/autoclick/restore) + state ownership
S3.4  verify + docs + snapshot
--- optional ---
S3.3b velext math + writer + -aimtest -> ovl_aim.cpp
```

Mỗi bước: **build_fast → build.bat → -aimtest → -srvtest → 8 tab → in-game → snapshot**. Không gộp bước, không sửa source trước khi plan được duyệt.
