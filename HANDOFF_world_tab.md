# HANDOFF — World tab (Đợt 2 "Visual & World control") — 2026-10-19

Trạng thái: **code xong, build sạch, self-test offline PASS**; các mục cần **mắt người**
(có/không hiệu ứng hình) nằm ở mục 7 — user chạy. Rollback point: `backup\overlay_WORLD_20261019.cpp`
+ `backup\ext_overlay_WORLD_20261019.exe` (trước khi làm tab World).

---

## 1. Việc đã làm

| File | Nội dung |
|---|---|
| `src\offsets.h` | +30 field Đợt 2: Lighting (ClockTime/Brightness/Ambient/OutdoorAmbient/Diffuse/Specular/Exposure/FogStart/FogEnd/FogColor/GlobalShadows), Atmosphere (Density/Haze/Glare/Color/Decay), effects (Enabled/Intensity/Size/Brightness/Contrast/TintColor), Terrain (GrassLength/WaterReflectance/WaterTransparency/WaterWaveSize/WaterWaveSpeed/WaterColor), Player (MinZoomDistance/MaxZoomDistance) |
| `src\offsets.cpp` | parse trong `fill()`; key thiếu → 0 → feature tự no-op |
| `src\overlay.cpp` | module World sau `mouseClipRestore`: cache instance (`wLighting/wTerrain/wAtmosphere/wBloom/wBlur/wCC/wLocalPlr`, retry ≤1/s, verify class 2s/lần), `worldTick()` (11 feature), `worldRestoreAll()`; hooks: place đổi, DataModel đổi, panic, `done`, game-closed, exit cuối — cạnh mọi chỗ `hitboxRestoreAll(m, o)` |
| `src\overlay.cpp` (UI) | tab 4 thay placeholder: giữ `BeginChild("##world", …, 330)` + `markChildArea()`, `hitMark()` sau **mọi** widget; dòng trạng thái `Lighting 0x… / not found`, `Terrain 0x…`, cờ atmo/bloom/blur/cc/player |

Luật ghi §0.2: chỉ ghi khi tick; seed original trước lần ghi đầu; untick → write lại
original + xoá state; diff-write ≤ 4 Hz cho hầu hết field; **FOV ghi mỗi frame** (đúng spec).
Instance bị thay (place mới / respawn) → original cũ bị **drop, không bao giờ ghi vào địa chỉ chết**.

## 2. cfg key mới

Theo spec: `world.full`, `world.bright`, `world.fog`, `world.shadows`, `world.expo`,
`world.clock`, `world.fx`, `world.atmo`, `world.water`, `world.grass`, `world.fovon`,
`world.fov` (70..120), `world.zoom`.

**+2 key phát sinh** (báo cáo như deviation): `world.expoval` (-2..2) và `world.clockval` (0..24)
— spec chỉ liệt kê cặp toggle cho Exposure/Time nhưng UI cần slider giá trị, và
`sliderFloatCfg` cần key riêng để lưu. Clamp đủ trong `loadAimCfg()`.

## 3. Self-test (output nguyên văn)

- `build_fast.bat` → `build overlay exit: 0`, exe mới `6,884,497` bytes (10:18 19/10).
  `-Wall -Wextra -fsyntax-only`: không có cảnh báo mới từ code world.
- `ext_overlay.exe -aimtest` → exit 0, dòng cuối `[5] all mapping cases PASS`.
- Sandbox `%TEMP%\opencode\worldtest` (cfg riêng, không đụng cfg user):
  - `ext_overlay.exe -diag -t 8` (game minimized, mọi world feature off) → exit 0,
    `world lighting=… terrain=… atmo=0 fx=0/1/0 plr=… vals bright=0.00 …`, exit không write gì.
  - `ext_overlay.exe -diag -nohide -t 8` với **chỉ** `world.fovon=1, world.fov=100`:
    - diag giữa chừng: `vals … fov=100.0deg(raw 1.7453) clock=-1.66e+15(=8.00 mod 24)`
      → FOV override sống, đã đổi đơn vị đúng (100° = 1.7453 rad).
    - exit: `world restore all (orig bright=0.00 fogStart=0 fov=70.0deg(raw 1.2217))`
      → restore đúng dword gốc.
  - Đọc lại sau run (read-only): `cam FOV @+304 f32=1.22173 +12=1.22173` → **cả 2 slot FOV
    đã về nguyên trạng**; `Lighting+184 = 0xd8bcf300` không bị đụng (test này clock off).
- Live read-only (game thật, pid 25884, version `11f155f3450f4c8e`):
  `Lighting 0x2b786464000`, `Terrain 0x2b9567bcce0` (GrassLength 0.8, WaterReflectance 0.3,
  WaterTransparency 0.5, WaterWaveSize 0.5, WaterWaveSpeed 10, WaterColor (0.243,0.298,0.235)),
  `Blur` có, Bloom/CC/Atmosphere **không tồn tại** trong game này (feature tự skip),
  `Player MinZoom 0.5 / MaxZoom 15`.

## 4. Phát hiện quan trọng: đơn vị FOV

`Camera.FieldOfView @+304` trên build này lưu **RADIANS**, không phải degrees
(`1.22173 = 70°` chính xác; thêm 1 bản copy y hệt tại `+316`, độc lập với +304 trong 400 ms
— tool `ext_fovpair.exe` kiểm chứng). Fix trong `worldTick`: seed đọc raw, nếu
`0 < raw ≤ 3.2` → chế độ radians (slider degrees → `×π/180`), else degrees như cũ;
**ghi cả +304 lẫn bản copy +12** (chỉ khi copy *bằng đúng* dword với +304 lúc seed) để chắc
slot nào renderer đọc cũng dính; restore cả hai. Diag in `70.0deg(raw 1.2217)` cho dễ đọc.

## 5. Phát hiện quan trọng: ClockTime của game này

`Lighting+184` chứa `-1.6620149e15` (0xd8bcf300), không đổi theo thời gian — đúng kiểu
"đóng băng giờ" (giá trị mod 24 = **8.00 h**, float ở mức 2^50 nên engine cộng dt vào
không đổi). Đã thử: (a) hold `6.0` vào slot 6 s → engine **không** phản ứng (không tự
advance, `SunPosition/LightDirection` đứng yên, không crash); (b) 8 slot float "giờ" khác
trong block Lighting cũng không slot nào làm sun nhảy. **Cảnh báo:** các test này chạy khi
cửa sổ Roblox đang **minimized** — engine throttle render/lighting khi minimized nên
"không phản ứng" chưa kết luận được là do event-driven hay do slot. Việc cần user: test
Time trong game **đang foreground** (mục 7). Write vẫn giữ theo đúng spec (theo slot
ClockTime 184, có save/restore dword) — không ghi vào chỗ khác, không crash khi đã thử.

## 6. `[I]` / fallback / điểm chưa verify

| # | Điểm | Ghi chú |
|---|---|---|
| 1 | **Terrain.WaterColor (464)** | `[I]` — cần UI màu, panel không host được popup (xem color-bar note tab Visual), để đợt palette |
| 2 | `world.expoval`, `world.clockval` | 2 key phát sinh (mục 2) |
| 3 | ClockTime trong game này | slot hợp lệ nhưng engine chưa cho thấy phản ứng (mục 5); slider vẫn hoạt động theo spec, cần mắt xác nhận |
| 4 | Bloom/CC/Atmosphere | không có instance trong game này → feature no-op im lặng (đúng thiết kế "key/instance thiếu → skip") |
| 5 | FOV khi overlay **chưa từng** resolve camera | `camAddr=0` khi overlay start lúc game không foreground (loop fast-path `continue` bỏ qua camera refresh); tick vào FOV chỉ sống sau khi panel hiện lần đầu trong game foreground. Mở panel 1 lần là đủ (đúng luồng dùng thật); không sửa core loop ở đợt này |
| 6 | Hiệu ứng hình từng mục (sáng/tối, sương, nước…) | write đã chứng minh "landed + restore" bằng readback; **mắt người** chưa xác nhận (game minimized suốt phiên) → mục 7 |

## 7. Checklist in-game (user chạy, game phải ở foreground)

1. Mở overlay (K nếu đang ẩn; panel phải đang mở rộng — bấm ▼ nếu bị thu gọn) → tab World:
   dòng trạng thái phải hiện `Lighting 0x…` + `Terrain 0x…`.
2. Từng mục: tick → xem hiệu ứng → untick → **hình phải về như cũ**:
   Fullbright (sáng gắt + ambient trắng), Anti-fog, No shadows, Exposure ±, Time (đổi 8→6→14h,
   xem nắng/trời đổi — nếu không đổi gì thì ghi nhận "game đóng băng giờ"), đổi nước/cỏ ở chỗ có nước/cỏ.
3. FOV: tick, kéo 70→120 → góc nhìn phải rộng ra rõ; untick → về 70. (Không được xảy ra: hình
   méo kiểu vài nghìn độ = dấu hiệu sai đơn vị → báo ngay.)
4. Far zoom: lăn chuột ra xa → phải kéo được xa hơn 15 (giới hạn gốc của game này).
5. Exit overlay (phím `\`) trong lúc 2-3 mục đang bật → hình về bình thường; `-diag` có
   `world restore all (orig …)`.
6. Không được có dấu hiệu 268 / kick khi bật/tắt liên tục vài lần.

## 7b. Menu UX mới (2026-10-20): tam giác thu gọn — **INSERT đã bị bỏ hẳn**

- **INSERT không còn tồn tại** (keybind + toggle + hint đều đã xoá). cfg `menu.show` /
  `menu.key` = di sản, bị bỏ qua khi load (không lỗi). Xem `HANDOFF_menu_collapse.md`.
- Panel **luôn tồn tại**. Bấm **▼ ở góc trái title bar** để thu gọn / mở rộng (bấm đúp
  title bar cũng vậy — hành vi chuẩn của ImGui). Thu gọn → pill `▼ ext menu` rộng ≥250px.
- Trạng thái thu/mở nhớ trong `overlay.cfg` key **`menu.collapsed`** (0/1), ghi lại đúng
  frame khi đổi. Mặc định 0 (mở) khi key chưa có.
- **K** = ẩn/hiện TOÀN BỘ overlay (cfg `toggle`) — **không đổi**. `\` = quit hẳn (restore).
- Panic (`panic.key`, mặc định DELETE) giờ **thu gọn** panel và trạng thái đó cũng được ghi
  vào `menu.collapsed`.
- Vòng FOV luôn vẽ mỗi frame khi cửa sổ overlay hiện (indicator aim) — nếu "chỉ thấy vòng
  FOV thì không thấy panel": thử K (bật/hiện overlay) rồi xem panel đang collapsed hay mở.
- Overlay **single-instance** (mutex `ext_overlay_singleton`): tắt bản đang chạy trước khi
  chạy bản sandbox/verify.
- Verify tự động không cần chuột (cold-start bằng cfg — dùng cho phiên sau):
  `menu.collapsed=1` → diag `size=250,19 hits=1 collapsed=1`; `menu.collapsed=0` →
  `size=344,253 hits=17 collapsed=0`; cfg `menu.size` **không** bị ghi đè bởi trạng thái
  collapsed (guard `g_menuH > 60`).
- **Gotcha cho phiên test sau:** click/chuột **inject** từ PowerShell vào overlay có thể
  lệch hoặc hụt do DPI-virtualization (process aware vs unaware — toạ độ set ≠ toạ độ
  process thấy trên màn hình scale ≠100%). Đừng kết luận UI hỏng từ click inject — hãy
  cold-start bằng cfg (đáng tin) và để **user click thật** khi game foreground.

## 8. Tool dựng trong phiên này (read-only / tự restore)

| Tool | Việc |
|---|---|
| `ext_worldprobe.exe` (`src\worldprobe.cpp`) | dump vùng Lighting/Terrain/Player/Camera + quét "giờ"/FOV; `--try-clock`: ghi thử 4-8 B vào candidate rồi restore, xem sun phản ứng |
| `ext_clockhold.exe` (`src\clockhold.cpp`) | hold giá trị vào Lighting+184 vài giây, đếm lần engine tự ghi lại / sun nhảy, restore cuối |
| `ext_fovpair.exe` (`src\fovpair.cpp`) | kiểm chứng +304 / +316 độc lập hay sync (kết quả mục 4) |

Build lẻ: `g++ -std=c++17 -O2 -static -o ext_worldprobe.exe src\worldprobe.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi`
(tương tự cho 2 tool kia). Log: `worldprobe_out3.txt`, `worldprobe_tryclock.txt`, `clockhold_out.txt`.
Dọn dẹp được: `wshot.ps1` (script chụp cửa sổ game — hiện vô dụng vì cửa sổ minimized).
