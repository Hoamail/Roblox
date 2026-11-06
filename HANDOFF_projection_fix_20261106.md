# HANDOFF — ESP/projection fix (2026-11-06, build `f0ea5da9412f4e35`)

Tài liệu này để **một AI khác tiếp tục công việc** mà không phá code. Đọc hết trước khi sửa
một dòng nào. Mọi con số trong đây là **số đo thật** (log/`ext_projcheck`), không phải suy đoán.

---

## 0. TL;DR

- ESP "không hoạt động" **không phải lỗi cache**. Nguyên nhân: **projection sai đơn vị** —
  `Camera.FieldOfView` (offset 296) trên build này lưu **RADIANS** (`1.2217` = `70°`),
  code cũ đọc như **độ** → `tanHalf = 0.0106` → phóng ~66× → mọi box/name rơi ngoài màn hình
  (đo thật: nhân vật cách camera 16.1 studs chiếu ra `y≈4434` trên client cao 777).
- Đồng thời `VisualEngine.ViewMatrix` (offset 432) — thứ trước đây bị kết luận là "chết" —
  **SỐNG trên build này** và khớp chính xác với camera. Đã đảo thứ tự: **ưu tiên VE matrix**
  khi nó qua được phép kiểm, fallback về camera-derived.
- Đã sửa: `camFovDegrees()` (chuẩn hoá rad/deg), gate chặt cho VE matrix
  (`camViewMatrixSane` + `camViewMatrixAgrees`), aim focal dùng cùng hàm chuẩn hoá,
  sensitivity calibration dùng cùng hàm.
- **Chưa xác minh end-to-end trong game** (lúc viết, Roblox không ở trong place:
  `model=0x0`, `PlaceId=0`, server chỉ có 1 player). Cần người ngồi máy test theo §7.
- AI trước **không làm hỏng gì** (§5). Thay đổi của nó là thật và có ích; đã được hardening.

---

## 1. Trạng thái hiện tại

| Hạng mục | Trạng thái | Bằng chứng |
|---|---|---|
| Build `ext_overlay.exe` | PASS | `build_fast.bat` exit 0, sha256 `287ADCE93C58` (13:12) |
| `-aimtest` | PASS, 0 `FAIL` | `=== done ===`, exit 0 |
| `-srvtest 0` | PASS | `=== done (PASS) ===` |
| `-offtest offsets_fixtures` | PASS | `fails=0 keys=58` |
| `-offtest offsets_fixtures\negative` | FAIL đúng như thiết kế | exit 1, `FakeDataModel.Pointer=0` |
| `build.bat` (toàn bộ tool) | PASS | `TOTAL_EXIT=0`; `build overlay exit: 0`, `build projcheck exit: 0` |
| Projection nguồn | `vm=1` (VE matrix) | `-diag` in `vm=1 vp=0,23,1280,777` |
| ESP trong game | **CHƯA XÁC MINH** | cần Roblox ở trong place + có character |
| Aim trong game | **CHƯA XÁC MINH** | như trên |
| `-verify` | FAIL ở `PlaceId` | người dùng **không ở trong place** lúc đo (`model=0x0`) |

`-verify` FAIL hiện tại **không phải lỗi code**: `DataModel.PlaceId` (384) đọc ra 0 vì chưa vào
place. Nếu vào place mà vẫn 0 thì mới là key chết → xem §8.1.

---

## 2. Bằng chứng gốc (đọc kỹ, đây là chỗ người ta hay chẩn đoán sai)

### 2.1 Log chứng minh projection sai

`aim_diag.log`, chạy `ext_overlay.exe -diag -nohide -t 6` (12:46, lúc **đang** có 8 player):

```
camera changed old=0 new=00000266af0a7ef0 pos=-3746.42,114.29,5356.50
proj src=2 client=0,23 1280x777 center=640,388 self=16.1studs screen=636,4434 others=7 onscreen=4 espOn=1 box=0 dist=1 name=1
entity refresh dm=... players=... local=... cache=8 ents=8
```

Đọc dòng này:
- `cache=8 ents=8` → **cache hoàn toàn OK**, 7 player khác + self.
- `self=16.1studs` → third person bình thường (camera cách nhân vật 16 studs).
- `screen=636,4434` → **X đúng** (tâm 640), **Y sai kinh khủng** (client cao 777).
- Kết luận: cache đúng, `ents` đúng, chỉ có **projection** sai. 4 entity "onscreen" thực chất chỉ
  là "nằm trước camera" (`screenW2S` trả true khi `w>0.1`), toạ độ pixel của chúng cũng văng.

### 2.2 Số đo quyết định

`ext_projcheck.exe` (tool mới, chạy khi Roblox đang mở):

```
client=0,23 1280x777
camera=0x2668f1f15f0 rot=1 pos=1 fov=1
camera pos=-291.23 303.57 -739.52 fovRaw=1.2217 fovDeg=70.00
rot9 = 1.000 0.000 0.000
       0.000 1.000 0.000
       0.000 0.000 1.000
basis right=1.000 0.000 0.000 up=0.000 1.000 0.000 fwd=-0.000 -0.000 -1.000
VE=0x2668f7b60d0 matrix=1 sane=1
  ve[0] =     0.8673     0.0000     0.0000   252.5858
  ve[1] =     0.0000     1.4281     0.0000  -433.5385
  ve[2] =     0.0000     0.0000     0.0000     0.1007
  ve[3] =     0.0000     0.0000    -1.0000  -739.5240
```

Giải mã (đây là toàn bộ chìa khoá của fix):

1. `fovRaw = 1.2217` **radian** = `1.2217 × 57.29578 = 70.00°`. Không phải 1.22 độ.
2. `ve[0][0] = 0.8673` và `0.8673 = 1/(aspect·tan(fov/2)) = 1/(1.6474 × 0.7002)`.
   `1.6474 = 1280/777` → đúng aspect client, đúng FOV 70°.
3. `ve[1][1] = 1.4281 = 1/tan(35°)` → khớp.
4. Hàng w `ve[3] = (0,0,-1)` = forward, và `m15 = -739.5240 = -eye.z` → `w(eye) = 0`.
5. `abs(252.5858) = 0.8673 × 291.23 = dot(row0, eye)` → eye chiếu đúng tâm.

Tức là **ma trận trong memory chính là view*projection thật của renderer** trên build này.
Trên build trước (`5a21b87bbc2641cf`) cùng key đó là blob chết (`row2=(0,0,0,0.0998)`, 16 float
không đổi qua 12 mẫu) — nên **không được tin key, chỉ được tin phép kiểm**.

### 2.3 Vì sao X đúng mà Y sai (dấu hiệu nhận dạng bẫy FOV)

Với `fov = 1.22°`, `tanHalf = 0.0106`:

```
ndcY = dot(d,up) / (tanHalf · depth)
```

nhân vật ở gần trục ngang (`dot(d,right) ≈ 0`) nên `x` vẫn ~0 → **X đúng**; còn lệch dọc vài
studs bị chia cho 0.0106 → **Y nổ**. Bất cứ khi nào thấy "X đúng, Y văng hàng nghìn px",
nghi ngay FOV/aspect, **không** nghi cache.

---

## 3. Ba thay đổi đã làm (và vì sao)

### 3.1 `src/ovl/ovl_core.h` — `camFovDegrees()`

```cpp
inline float camFovDegrees(float raw) {
    if (!(raw > 0.0f)) return 0.0f;
    return raw < 3.2f ? raw * 57.29577951308232f : raw;
}
```

- Roblox clamp `FieldOfView` trong `[1, 120]` độ; dạng radian của 1°..120° là `0.017..2.09`.
  Vì vậy `< 3.2` ⇒ radian. (Trường hợp game đặt FOV 1–3° là không thực tế; nếu gặp, log sẽ lộ.)
- `ovl_world.cpp` **đã** tự xử lý việc này từ trước bằng `kFovRadMax` (FOV slider ghi đúng rad).
  Projection và aim thì chưa — đó là lý do chỉ ESP/aim chết còn slider FOV vẫn đúng.

### 3.2 `src/overlay.cpp` — chọn nguồn projection

Khối `float vm[16]{}; ... vmSrc` (khoảng dòng 3015–3060). Logic mới:

```
1) Đọc Camera.Rotation (192) + Camera.Position (228) + FieldOfView (296) một lần
   → camFov = camFovDegrees(fov) → camViewBuild(camView, rot, cam, camFov)
2) Đọc VisualEngine.ViewMatrix (432). Chấp nhận nếu:
     camViewMatrixSane(vm, cam, vw, vh)
     && (!camView.ok || camViewMatrixAgrees(vm, camView))
   → vmSrc = 1
3) Ngược lại, nếu camView.ok → camViewMatrix(camView, aspect, vm) → vmSrc = 2
4) Không được gì → vmSrc = 0, log view-no-projection
```

`-diag` in `vm=` = 0/1/2 (0 không có, 1 VE, 2 camera-derived). **Đây là chỉ báo đầu tiên phải xem
khi ESP/aim sai.**

### 3.3 `src/ovl/ovl_camview.h` — gate chặt hơn

`camViewMatrixSane()` (chống blob chết) kiểm:
1. `|w row|` hữu hạn, `1e-6 < |w row| < 1e6`.
2. `|row0| · aspect ≈ |row1|` (sai khác ≤ 2×). Cả hai dạng ma trận đều thoả: engine đặt
   `tanHalf` trong row0/row1, `camViewMatrix()` của ta đặt trong hàng w — nhưng tỉ lệ
   `|row0|/|row1| = 1/aspect` là bất biến.
3. `w(eye) ≈ 0` (≤ `2·|w row|`) → ma trận phải **neo vào camera hiện tại**.
4. Điểm `eye + 20·f̂` phải chiếu vào **tâm client ±5%**.

`camViewMatrixAgrees()` kiểm `|dot(fwd_matrix, fwd_camera)| ≥ 0.98` — khoá ma trận vào
`Camera.Rotation` đọc **cùng frame**, nên blob "trông hợp lệ" nhưng không xoay theo camera vẫn bị loại.

`camViewMatrix()` (camera-derived) **không đổi**: đặt `tanHalf` ở hàng w, row0 = `right/aspect`,
row1 = `up`. Đừng "chuẩn hoá" nó theo kiểu ma trận engine — `screenW2S` đang dựa vào dạng hiện tại.

### 3.4 `src/ovl/ovl_aim.cpp` — focal của velext mouse mapping

```cpp
float fovDeg = camFovDegrees(fovV);              // trước: dùng thẳng fovV
if (fovDeg > 10.0f && fovDeg < 150.0f) { ... focal = (vh*0.5f)/tanHalf; }
```

Trước fix: `fovV = 1.22` không qua điều kiện `> 10` ⇒ `focal = 0` ⇒ `mapped = false` ⇒
**mỗi frame rơi về gain legacy** (`aim-map-fallback`). Nghĩa là nhánh "velext mapping" chưa
bao giờ chạy trên build mới. Sau fix: `focal = 388.5/0.7002 = 554.9` (client 777).

### 3.5 `src/overlay.cpp` — calibration độ nhạy (`calibPhase==1`)

Cùng bẫy: trước đây `vf > 10 && vf < 150` nên luôn dùng `70.0f` mặc định. Nay dùng
`camFovDegrees(vf)`; nếu game đổi FOV (World tab) thì calibration vẫn đúng.

---

## 4. Test đã thêm (đừng xoá)

`-aimtest` mục `[6]` giờ in:

```
centre=1 edgeX=1 edgeY=1 behind-rejected=1 client-origin=1 dead-matrix-rejected=1
fov-radians=1 vp-accepted=1 vp-moved-rejected=1 vp-agrees=1 vp-turned-rejected=1 (stale-identity=1)
[6] projection PASS (fov70 aspect16:9 tanHalf=0.700208)
```

Nghĩa của từng cái (mỗi cái là một regression thật đã từng xảy ra):
- `fov-radians`: build với `camFovDegrees(1.22173)` phải **giống từng phần tử** với build 70°.
- `vp-accepted`: ma trận dạng engine (chuẩn, `tanHalf` ở row0/row1) phải qua `camViewMatrixSane`.
- `vp-moved-rejected`: cùng ma trận + eye lệch 30 studs → phải bị loại (`w(eye)≈0`).
- `vp-agrees` / `vp-turned-rejected`: `camViewMatrixAgrees` phải khớp khi cùng hướng, loại khi lệch 90°.
- `stale-identity`: 4x4 identity phải bị loại (không có hàng w).

**Bẫy đã từng gặp khi viết test này:** truyền `float rotId[9]` vào hàm nhận `float mt[16]` →
đọc tràn stack, kết quả test nhảy lung tung. Luôn dùng mảng 16 phần tử.

---

## 5. AI trước đã làm gì — có hỏng không?

**Trả lời ngắn: không hỏng. Thay đổi của nó là thật và có ích; đã được hardening.**

### 5.1 Thay đổi duy nhất còn lại trong code

`src/ovl/ovl_cache.cpp`, trong `EntityCtx::refreshEnts()`, nhánh player **đã có** trong cache:

```cpp
// TRƯỚC (baseline backup\S3POST_20261020):
if (!m.read(p + o.playerModelInstance, model) || !SDK::plausiblePtr(model)) {
    clearResolvedParts(e);   // mất prim -> ents rơi khỏi danh sách -> ESP nháy
    return true;
}

// AI ĐÓ SỬA THÀNH:
if (!! đọc model thất bại) {
    return true;             // giữ part cũ, không rơi entity
}
```

Đánh giá:
- **Đúng hướng**: `Player.ModelInstance` đọc hụt một mẫu là chuyện bình thường khi Roblox
  re-rig character; xoá part ngay làm `ents` nhảy `7 → 5 → 7` (flicker).
- **Lỗi biên tôi đã vá**: nhánh `return true` bỏ qua **toàn bộ** phần chứng minh part còn sống
  (`partGone` / `Part.Parent == Model`). Nếu character bị destroy mà `ModelInstance` vẫn đọc hụt,
  box ma (position đóng băng) treo vô hạn. Nay nhánh đó vẫn kiểm:
  - `rootPart.Parent != e.model` → `clearResolvedParts(e)`, hoặc
  - `Primitive.Position` không đọc được / NaN → `clearResolvedParts(e)`.
- `clearResolvedParts()` set `e.model = 0`, nên lần đọc model thành công kế tiếp vẫn đi đúng
  đường `model != e.model` → re-resolve.

### 5.2 Những thứ khác nó (hoặc công cụ của nó) để lại

- `src/ovl/ovl_aim.c` — file `.c` rác nó tạo ra: **đã không còn** (tự xoá hoặc đã bị xoá).
  Không có trong `build.bat`/`build_fast.bat` nên không ảnh hưởng build.
- `overlay.cfg` mtime 12:46: file runtime tự ghi khi đổi UI. Không có key lạ.
- Không có file source nào khác bị nó sửa (kiểm bằng mtime toàn cây: sau 10:40 chỉ có file của
  session này).

### 5.3 Chẩn đoán của nó sai ở đâu

Nó kết luận "cache rơi entity 7→5/6→7 ⇒ ESP mất box". Cái đó **có thật** nhưng **không phải
nguyên nhân** của "ESP không hoạt động": lúc đó projection sai 66×, nên **box không thể hiện dù
cache đầy đủ**. Nó không có bước đo projection (không có `ext_projcheck`, không đọc `fovRaw`),
nên đoán vào cache. Bài học: **đo trước, sửa sau**; `proj-check`/`entity-refresh` là hai dòng log
phân biệt được hai nguyên nhân này.

### 5.4 Ghi chú về git

Repo baseline rất cũ (`6d2b161`), toàn bộ công việc đang nằm ở working tree chưa commit
(`src/ovl/*` phần lớn là untracked). **Đừng `git checkout` file nào** — mất trắng. Snapshot chuẩn
là `backup\S3POST_20261020\`. `.autocommit` không có ⇒ **không tự commit**.

---

## 6. Bản đồ file liên quan

| File | Vai trò |
|---|---|
| `src/overlay.cpp` | main loop; khối chọn projection (~3015–3060); diag `proj-check`/`proj-raw`; `-aimtest` mục [6] |
| `src/ovl/ovl_camview.h` | `CamView`, `camViewBuild`, `camViewMatrix`, `camViewMatrixSane`, `camViewMatrixAgrees` |
| `src/ovl/ovl_core.h` | `diagRate`, `qpcSeconds`, `qpcNs`, **`camFovDegrees`** |
| `src/ovl/ovl_view.h` | `screenW2S` (quy ước: `sx = left + vw/2·(1+x/w)`, `sy = top + vh/2·(1−y/w)`) |
| `src/ovl/ovl_esp.cpp` | vẽ box/name/hp/skel/line/oof + radar; gate `cfgEspOn`, `cfgEspMaxDist`, `h ∈ [4,2000]` |
| `src/ovl/ovl_cache.cpp` | 3 tầng cache entity (players + NPC); phần AI trước sửa ở §5 |
| `src/ovl/ovl_aim.cpp` | aim tick, trigger, autoclick; focal velext mapping (§3.4) |
| `src/projcheck.cpp` | **tool mới** `ext_projcheck.exe`: in số live của projection |
| `src/ovl/ovl_world.cpp` | FOV slider — đã tự xử lý radian (`kFovRadMax`) từ trước |
| `build.bat` / `build_fast.bat` | danh sách nguồn **tường minh**; đã thêm `ext_projcheck` vào `build.bat` |
| `FINDINGS_overlay_internals.md` | §6.1 mới: toàn bộ kiến thức projection + bẫy đơn vị |
| `AGENTS.md` | gotchas FOV radians + VE matrix + `ext_projcheck` |

---

## 7. Việc PHẢI làm tiếp (theo thứ tự)

### 7.1 Xác minh ESP trong game (bắt buộc, chưa ai làm)

Điều kiện: Roblox **đang ở trong một place**, có character (không phải màn hình home), server có
ít nhất 1 player khác (hoặc bật NPC).

1. Đóng overlay đang chạy (mutex `ext_overlay_singleton`).
2. `ext_projcheck.exe` → phải thấy `fovDeg ≈ 70` (không phải 1.22), `VE ... sane=1`,
   và với mỗi player: `dotR/dotU/depth` + `cam=px,py/1`.
   - Self ở third person phải có `cam ≈ (640, 380..450)` trên client 1280×777.
   - Player đứng bên phải crosshair phải có `cam px > 640`.
3. `ext_overlay.exe -diag -nohide -t 6` → log phải có:
   ```
   entity refresh ... cache=N ents=N        (N > 1 nếu server có người)
   proj src=1 client=0,23 1280x777 ... self=..studs screen=~640,~400 others=.. onscreen=..
   ```
4. Mở menu (INSERT) → tab **Visual** → bật **Box** (config hiện tại `esp.box=0`, chỉ name/dist/hp
   được bật — nếu chỉ nhìn "không có box" thì đó là config, không phải bug) → box phải **bám đúng**
   player, không lệch, không nhảy khi xoay camera.
5. Xoay camera 180°: box phải biến mất/ hiện đúng phía, `proj-raw` phải đổi `dotR/dotU/depth`
   tương ứng. Nếu box đứng yên khi xoay → nghi `camViewMatrixAgrees` loại VE và fallback sai.

### 7.2 Xác minh aim

- Mode 1 (memory writer, `overlay.cfg: aim.mode=1`): giữ RMB khi có target → camera phải quay mượt
  về target; nhả RMB → **mouse look phải còn hoạt động** (đây là bug cũ "camera đứng im").
- Mode 0 (mouse counts): lần đầu tiên nhánh velext mapping chạy thật (focal khác 0). Kiểm
  `aim-map-fallback` **không** còn xuất hiện trong `aim_diag.log`.
- Chỉ chạy **một** tool aim: đừng bật `velext` cùng lúc.

### 7.3 Kiểm `PlaceId`

`-verify` hiện FAIL ở `PlaceId (placeId@0x180)`. Nếu **trong place** mà vẫn 0:
- Key chết → cần scan lại offset (giá trị place id lấy từ log server, ví dụ `101770480176177`
  trong `srv job=... place=...`), `tools\offsets_diff.ps1` để so dump.
- Tác động nếu chết: `refreshEnts()` không phát hiện map hop ⇒ `loadHitboxProfile`,
  `hitboxRestoreAll`, `worldRestoreAll` không chạy khi đổi place. Không ảnh hưởng ESP.

### 7.4 Việc mở (chưa quyết)

| # | Việc | Ghi chú |
|---|---|---|
| 1 | Cadence aim mode 1 | Ta actuate 1 lần/snapshot; velext ≤250 Hz với `alpha = 1−exp(−dt/tau)`. Đo cảm giác thật rồi mới đổi |
| 2 | `aim.interval` (`cfgAimInterval`) | Đang 0; velext không có khái niệm này |
| 3 | `cfgAimSmooth` mặc định 8.67 | Velext dùng softness 10.8065 (`tau = 10.8065×8 ms`). Chỉ đổi nếu cảm giác lệch |
| 4 | `Camera.Viewport` (644)/`ViewportSize` (708) | Hiện **không dùng**; projection lấy client rect qua `gameClientRect()`. Đo lại nếu gặp game có black-bar |
| 5 | ESP `h ∈ [4,2000]` | Ngưỡng chặn box quá nhỏ/lớn; nếu player ở rất xa bị mất box thì nới |

---

## 8. Quy trình gate (chạy đủ trước khi báo "xong")

```powershell
cd C:\Users\adminn\ext_project
.\build_fast.bat                                    # exit 0 + sha256
.\ext_overlay.exe -aimtest                          # exit 0, 0 dòng FAIL, [6] PASS
.\ext_overlay.exe -srvtest 0                        # === done (PASS) ===
.\ext_overlay.exe -offtest offsets_fixtures         # fails=0 keys=58
.\ext_overlay.exe -offtest offsets_fixtures\negative# FAIL, exit 1  (đúng thiết kế)
.\build.bat                                         # toàn bộ tool exit 0
.\ext_projcheck.exe                                 # cần game: fovDeg ~70, sane=1
.\ext_overlay.exe -verify                           # cần game + đang trong place: verify: PASS
.\ext_overlay.exe -diag -nohide -t 6                # vm=1|2, proj-check hợp lý
```

Kiểm line ending (bắt buộc, file LF-only, không BOM):

```powershell
foreach($f in @('src\overlay.cpp','src\ovl\ovl_cache.cpp','src\ovl\ovl_camview.h','src\ovl\ovl_core.h','src\ovl\ovl_aim.cpp','src\projcheck.cpp')){
  $b=[IO.File]::ReadAllBytes($f); $cr=0; foreach($x in $b){ if($x -eq 13){$cr++} }
  "{0}: CR={1}" -f $f,$cr }
```

---

## 9. Bẫy — đọc trước khi sửa

1. **Không hardcode offset.** Mọi thứ qua `Offsets`. Dump hiện tại: `Camera.Rotation=192`,
   `Position=228`, `FieldOfView=296`, `CameraSubject=176`, `VisualEngine.ViewMatrix=432`,
   `VisualEngine.FakeDataModel=2800`, `Primitive.Position=212`.
2. **Không tin sự tồn tại của key = key còn sống.** `ViewMatrix` sống ở build này, chết ở build
   trước. `FieldOfView` đổi **đơn vị**. `StatsItem.Value` trong dump là rác (52434).
3. **`Camera.Rotation` không có fallback** (cố ý): offset cũ 200 sẽ đè `Camera.Position` → camera
   đứng im. Nếu `cameraRotation == 0` thì writer aim tự tắt (`aim.mode=0` + banner đỏ).
4. **Không đổi `camViewMatrix()` sang dạng ma trận engine** — `screenW2S` phụ thuộc dạng hiện tại.
5. **`vm=0` là chế độ ESP tắt hoàn toàn** (`haveVm` false → không gọi `espCtx.draw`). Đừng "vẽ tạm"
   bằng ma trận chưa qua gate: box sẽ nằm sai chỗ và người ta lại đi sửa cache.
6. **Một instance overlay** (mutex). Không chạy `velext` + `ext_overlay` cùng lúc.
7. **Không attach Frida vào velext** (đã 2/2 lần velext tự thoát ngay sau khi attach).
8. **`src/overlay.cpp` và `src/ovl/*` là LF-only, không BOM.** Dùng `edit`/`write`, đừng dùng
   PowerShell `Set-Content` mặc định (CRLF).
9. **`build.bat`/`build_fast.bat` liệt kê nguồn tường minh** — thêm file mới phải thêm vào **cả hai**
   (trừ tool standalone chỉ cần `build.bat`). Không dùng glob `src\*.cpp` (có ~25 `main()`).
10. **`ext_overlay.exe` đang chạy ⇒ build `exit /b 2`.** Không kill trước khi hỏi; chỉ kill khi
    chính mình vừa mở nó để test.
11. **Diag đi vào `aim_diag.log`** (không phải stdout). Dòng `menu tab=...` mới là stdout.
    Đọc log bằng `Get-Content aim_diag.log -Tail N`, đừng lọc stdout rồi kết luận "không có log".
12. **`proj-check` chỉ in khi `ents` không rỗng** — nếu chưa vào place (`ents=0`) thì im lặng,
    không phải lỗi.

---

## 10. Cách chẩn đoán nhanh (decision tree)

```
ESP không hiện?
├─ entity refresh ... ents=0            → cache/SDK: chưa vào place? model=0? xem -verify
├─ ents>0 nhưng không thấy box/name
│   ├─ -diag vm=0                       → không có projection: xem view-no-projection (cam? fov?)
│   ├─ -diag vm=1|2                     → projection OK ⇒ kiểm cfgEspOn / esp.box / esp.maxdist
│   └─ ext_projcheck fovDeg ≈ 1.2       → BẪY FOV RADIAN (§3.1) tái phát
└─ box hiện nhưng lệch vị trí
    ├─ lệch đều theo phương dọc         → aspect hoặc viewport (client rect) sai
    ├─ lệch khi xoay camera             → camViewMatrixAgrees/fallback, hoặc ma trận cache 1 frame
    └─ chỉ 1 player lệch                → part chết (parent != model) ⇒ cache re-resolve

Aim không quay / quay giật?
├─ aim-no-actuator                     → thiếu camAddr/cameraRotation/write handle (mode 1)
├─ aim-map-fallback                    → focal = 0 ⇒ FOV (§3.4)
├─ aim tick ... tgt=no                 → không có target: FOV gate (dùng vm), dist, team/dead filter
└─ world lặp y nguyên nhiều giây       → ghost target: cache liveness
```

---

## 11. Những gì đã thay đổi so với snapshot `backup\S3POST_20261020`

| File | Thay đổi |
|---|---|
| `src/ovl/ovl_core.h` | + `camFovDegrees()` |
| `src/ovl/ovl_camview.h` | `camViewMatrixSane()` chặt hơn hẳn; + `camViewMatrixAgrees()` |
| `src/overlay.cpp` | chọn projection: VE trước, camera-derived sau; + diag `proj-check`/`proj-raw`; calib dùng `camFovDegrees`; `-aimtest` [6] thêm 5 case |
| `src/ovl/ovl_aim.cpp` | focal dùng `camFovDegrees` |
| `src/ovl/ovl_cache.cpp` | diag `cache-stop` cho 4 nhánh return im lặng; hardening nhánh ModelInstance hụt (§5.1) |
| `src/projcheck.cpp` | **mới** — tool đo projection live |
| `build.bat` | + `ext_projcheck.exe` |
| `FINDINGS_overlay_internals.md` | + §6.1 |
| `AGENTS.md` | + gotcha FOV radians, VE matrix, `ext_projcheck` |

Không đổi: offset nào, key `overlay.cfg` nào, tab nào, layout menu, cơ chế Fly/Teleport/Spectate.

---

## 12. Câu hỏi mở (ghi lại để không "phát minh" lại)

1. Trên build này `Camera.ViewportSize` (708) có bằng client rect không? (Chưa đo; hiện dùng
   `gameClientRect` vì nó đúng cả khi cửa sổ windowed.)
2. `VisualEngine.Dimensions` (2832) có phải kích thước viewport thật? (Chưa dùng.)
3. `Camera.FieldOfView` còn radian ở các build sau không? → `camFovDegrees` xử lý cả hai, nhưng
   nếu Roblox đổi sang `[1..3]` độ thì heuristic sai; khi đó phải suy `tanHalf` từ chính ma trận
   (`|row1|` hoặc `|w row|`) và bỏ heuristic.
4. Aim mode 1 vs mode 0: build này mode 1 (writer) đã chạy đúng từ trước; mode 0 giờ mới có focal
   thật. Chưa so cảm giác giữa hai mode sau fix.

---

## 13. Trạng thái build cuối (cập nhật khi bạn chạy lại)

- `ext_overlay.exe` sha256 (12 ký tự): `F94F079FD5EE` — build 13:20 06/11/2026 (`build.bat` đầy đủ,
  `TOTAL_EXIT=0`, `build projcheck exit: 0`)
- `ext_projcheck.exe`: build cùng ngày. **Dòng `build.bat` của nó phải có `-Isrc`** —
  `src\projcheck.cpp` include `ovl/ovl_camview.h`, file đó include `"sdk.h"` tính theo thư mục
  `src\ovl\`, nên thiếu `-Isrc` sẽ `fatal error: sdk.h: No such file or directory`
  (đã từng xảy ra và làm `build.bat` exit 1 dù overlay vẫn build đúng).
- `-verify`: FAIL ở `PlaceId` **vì chưa vào place**, không phải regression
