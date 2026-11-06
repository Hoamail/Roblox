# PROMPT — Reverse engineering toàn bộ subsystem AIM + CAMERA ROTATION (ext_project)

> Dán nguyên file này vào chat AI mới. Nó tự chứa: bối cảnh, số đo đã có, bản đồ code,
> 9 luồng việc, format findings, gate không được phá, và định nghĩa "đã sửa xong".

---

## 0. Bối cảnh

Repo: `C:\Users\adminn\ext_project` (Windows, MinGW g++). Đây là **overlay external** cho
Roblox: đọc/ghi memory cross-process, **không inject, không gọi remote, không hook**. C++17.
`src\overlay.cpp` là khung menu + main loop; phần lớn logic đã tách sang `src\ovl\*.cpp`.

Đây **không phải project mới**: đã có lịch sử dài, snapshot chuẩn `backup\S3POST_20261020\`,
kiến thức đã xác minh trong `FINDINGS_overlay_internals.md` (đọc §1 trước khi làm gì khác),
runbook update Roblox trong `HANDOFF_roblox_update_playbook.md`.

**Mục tiêu duy nhất của phiên này:** tìm nguyên nhân gốc của lỗi "aim đóng băng camera" và
sửa nó. Không thêm feature. Không refactor ngoài phạm vi. Không đổi key `overlay.cfg`.

---

## 1. Triệu chứng (nguyên văn người dùng)

> "Giữ RMB ở chế độ first person để auto aim, nó quét được kẻ địch rồi thì chuột và camera cứ giữ nguyên vị trí đó,
> tiêu điểm ban đầu gắn vào chỗ ban đầu, không tự động đi theo kẻ địch. Lúc ấy màn hình cứng
> đờ. Xảy ra **thường xuyên**."

Diễn giải cần kiểm chứng (đừng mặc định cái nào đúng):

- (a) Game vẫn render bình thường (thấy cảnh), nhưng **hướng nhìn bị ghim**; mouse look vô hiệu (chế độ người chơi thứ nhất first person).
- (b) Aim **không bám theo world position** của target dù target vẫn di chuyển.
- (c) Tự hết khi thả key / đổi target. Không đoán trước được lúc nào dính.
- (d) Người dùng KHÔNG dùng chế độ silent-aim (mode 2), không spectate, không stunt.

---

## 2. Cấu hình runtime đang dùng (bám đúng cái này)

`overlay.cfg` (giá trị thực tế trên máy người dùng):

```
aim.enabled=1        # bật trong tab Combat mỗi phiên
aim.key=RMB          # keymode=0 => HOLD
aim.mode=1           # MEMORY WRITER (rotation), KHÔNG phải mouse counts
aim.pid=0            # model "velext": <=1 actuation / publish, gate 4 ms
aim.smoothing=7.50   # tau = 8ms * 7.5 = 60ms
aim.rate=500         # writer Sleep(1000/rate) = 2 ms
aim.deadzone=1       # px
aim.fov=169          # px
aim.sticky=10        # on
aim.sticktime=4      # s
aim.predict=0        # off
aim.interval=0       # paced = luôn true
aim.bone=0           # anchor = Head primitive
aim.maxdist=2000
```

Hệ quả: chỉ có **một** đường actuation được dùng — `writerThread` trong `overlay.cpp`.
Mode 0 (mouse counts) và mode 2 (viewport shift) là code chết trong phiên này, nhưng vẫn phải
review vì chúng chia sẻ state.

---

## 3. Đã đo được những cái này, cần kiểm chứng lại độ hiệu quả của các thước đo. Nghi ngờ tác dụng của các thước đo này

1. `src\ovl\ovl_aim.cpp` hiện tại **byte-identical** với `backup\S3POST_20261020\src\ovl\ovl_aim.cpp`
   (`sha256 1E2F475748EC5A2B1741A13E493AE2D62562FC31DE352ED9D7D3D05DAE3ADB4F`).
   => "AI trước sửa hỏng aim" là SAI. Thay đổi nằm ở `overlay.cpp` (thêm stunt/tp/spectate/tools),
   `offsets.cpp`, `ovl_cfg.cpp`.
2. Offsets live khớp game `version-5a21b87bbc2641cf` (`hash d1e29e3dd8265890`):

```
Camera.Rotation=192          Camera.Position=228         Camera.CameraSubject=176
Camera.Viewport=644          Camera.FieldOfView=296      Camera.CameraType=288
Primitive.Position=212       Primitive.Rotation=176      Primitive.AssemblyLinearVelocity=224
Primitive.Owner=536          BasePart.Primitive=376      Instance.Parent=96
VisualEngine.ViewMatrix=432  Instance.NameContainer=104  Instance.Name=0
```
3. `ext_overlay.exe -verify` → `verify: PASS`, chain đủ 10 mắt
   (dm/ws/cam/players/local/model/humanoid/hrp/prim/place). Chỉ warning
   `StatsItem.Value` (hỏng từ dump, chỉ ảnh hưởng ping ở tab Server).
4. Log live 12 phút (`build Nov 5 00:22:32`, `aim.mode=1`):

```
writer-reverted        = 0
writer write failed    = 0
writer-disabled        = 0
aim tick               = 180 dòng, world position ĐỔI mỗi giây
static-target run dài nhất (acted=1) = 0 s
camera invalid/changed = 4
entity re-resolve      = 205 model-change / 245 part-gone / 26 bone-gone
```
   => Trong cửa sổ đó KHÔNG tái hiện freeze. Cần bắt được lúc freeze thật (xem W7).
   Lưu ý: 3 kết quả đầu chứng minh **write có land** ở build hiện tại — nhưng đó là
   bằng chứng "memory đổi", KHÔNG phải bằng chứng "engine chấp nhận và render theo".
5. `ext_primcheck.exe` (read-only): `Instance.Parent(part)==Model` đúng 7/7 player,
   `Primitive.Owner → BasePart.Primitive` khớp 14/14. Rule liveness của cache là đúng.
6. 3 đường đã bịt (đừng revert; nhiệm vụ của phiên này là chứng minh **đủ hay chưa đủ**):
   - `cameraRotation` từng có fallback cứng `200` (đè `Camera.Position` 228/232 khi dump thiếu
     key) → nay default `0`, `0` = writer return + ép `aim.mode=0` + banner + log `writer-disabled`.
   - `aim.mode=1` từng rơi xuống inject mouse counts khi `camAddr==0` → nay nhánh chuột chỉ cho
     `cfgAimMode==0`; mode 1 thiếu camera chỉ log `aim-no-actuator`.
   - Cache: `Ent` giữ `rootPart/headPart/limbPart`, refresh kiểm `Instance.Parent(part)==model`,
     bone lệch root >30 studs bị bỏ (`aim-bone-stale`), xác chết (`hp<=0`) không re-resolve loop.

---

## 4. Bản đồ code phải đọc (đúng thứ tự, kèm số dòng hiện tại)

**`src\overlay.cpp`**
| dòng | nội dung |
|---|---|
| 190 | `qpcNs()` |
| 195 | `g_velLastActNs` (stamp actuation dùng chung mode 0/1) |
| 198 | `velextAlphaFromDt()` — `alpha = 1-exp(-dt/tau)`, `tau = 8ms*max(1,smoothing)`, dt clamp 0.1 |
| 213 | `velextActGate()` — gate cứng 4 ms, false = không actuate và **không** tiêu stamp |
| 790 | `g_snap` / `g_snapMu` / `g_snapEpoch` / `g_snapSeq` / `g_currentCam` |
| 797 | `invalidateAimSnapshot()` — bump epoch, `ready=false`, `camAddr=0` |
| 897 | `buildAimMatrix()` — look-at → 3x3, **columns = Right, Up, -Look** |
| 933 | `snapPos()` — predict/lead, `aim.predict=0` nên trả `s.tgt` |
| **953** | **`writerThread()` — TOÀN BỘ mode 1**: freshness → dot deadzone → nlerp → write |
| 1020 | nhánh `pid=0` (velext): 1 actuation / snapshot, `dot > 0.99996` = không ghi |
| 984 | nhánh `pid=1` (legacy slerp free-running) — dùng để A/B |
| 1096 | `Sleep(1000/rate)` cuối mỗi vòng writer |
| 1104 | `runAimSelfTest()` — `-aimtest`: chỉ test math, KHÔNG test engine |
| 2408..2437 | refresh camera chain mỗi loop + `g_currentCam`; fail → `camera invalid` |
| 2494 | `wantShow = nohide ? true : (visible && (fgPid == m.pid))` |
| **2510** | **`if (!wantShow) { Sleep(50); continue; }`** → panel ẩn/hết foreground = **không gọi aim tick** |
| 2550 | calibration F8: đọc `Camera.Rotation` khi game focused (đường dùng để suy ra convention) |
| 2906 | đọc `VisualEngine.ViewMatrix` **1 lần / frame** (trước aim tick) |
| 2929 | `aimCtx.tick(dl, vm, true, fgPid, typingNow)` |
| 2939 | `aimCtx.autoclickTick(...)`; sau đó `restoreViewportIfNeeded(false)` |

**`src\ovl\ovl_aim.cpp`**
| dòng | nội dung |
|---|---|
| 20 | `AimCtx::tick` — `wantAim = aimOn && cfgAimEnabled && fgPid==m.pid && !typingNow` |
| 76 | `evaluateEntity` — model/health/root/bone/predict/`w2s`/score |
| 194 | nhánh sticky (`stickPlayer`, `lockPos`, `sticktime`) |
| 297 | `if (wantAim && !haveTarget) invalidateAimSnapshot()` |
| 305 | `acted` + invalidate khi `distPx < aim.deadzone` |
| 319 | mode 2 viewport shift: ghi `Camera.Viewport = ((resW-rx)*2,(resH-ry)*2)` |
| 343 | publish snapshot (`tgt=bestTarget.world`, `cm=cam`, epoch/seq/qpc) |
| 359.. | mode 0: mouse counts, back-pressure `bp*`, cursor guard, MouseService resolve |

**Module khác (đều là writer chạy song song, phải kiểm tra tranh chấp)**
- `src\ovl\ovl_spectate.cpp` — ghi `Camera.CameraSubject` (thread riêng, chỉ khi `g_on`).
- `src\ovl\ovl_fly.cpp` — writer `AssemblyLinearVelocity` ~1e6 writes/s, đọc `Camera.Rotation`
  để lấy basis; **không** ghi rotation.
- `src\ovl\ovl_stunt.cpp` — ghi CFrame/basis HRP; đọc `Camera.Rotation` (`:749`), không ghi camera.
- `src\ovl\ovl_tp.cpp` — dash velocity + pin/restore `CanCollide`.
- `src\ovl\ovl_view.h` — `screenW2S` (ESP + aim dùng chung).
- `src\ovl\ovl_cache.cpp` — `EntityCtx::refreshEnts()` (250 ms), liveness part.
- `src\ovl\ovl_types.h` — `Ent` (prim/headPrim/limbPrim/rootPart/headPart/limbPart/humanoid/team).
- `src\offsets.cpp` — `fill()` / `validateOffsets()` / `ensure()` (live→pinned→newest→good→cache).
- `src\sdk.h`, `src\memory.h`, `src\ntwrite.cpp` (syscall write dùng chung).

**Tham chiếu model gốc:** `tools\research\velsample.cpp` + `FINDINGS_overlay_internals.md` §1
(aim) và §6 (bảng offset velext-era vs hiện tại — **velext lệch -8 byte** so với dump hiện tại;
đừng bê nguyên số của velext).

---

## 5. Chia luồng — làm đúng thứ tự, mỗi luồng có deliverable riêng

### W0. Baseline & repro protocol (làm trước, ~30 phút)
- Chạy và lưu output: `-aimtest`, `-offtest offsets_fixtures`, `-offtest offsets_fixtures\negative`
  (phải exit 1), `-srvtest 0`, `-verify`, `ext_primcheck.exe`; copy `overlay.cfg`, sha256 exe.
- Dựng kịch bản repro có kiểm soát: 1 target đi ngang đều, giữ aim 60 s, ở 60/144/240 fps nếu đổi
  được. Ghi `aim_diag.log` + quay màn hình (để đối chiếu thời điểm).
- Định nghĩa **"freeze" đo được** (bắt buộc, không chấp nhận cảm giác). Ví dụ:
  `aim tick` ≥5 dòng liên tiếp mà `world=` của target không đổi trong khi target thực di chuyển;
  hoặc `writer-deadzone` liên tục >1 s mà `dot` không tiến về 1.
- **Deliverable:** FINDINGS §0 — bảng baseline + protocol + tiêu chí freeze.

### W1. Write surface — ghi cái gì mới thực sự làm camera quay? (QUAN TRỌNG NHẤT)
Trả lời bằng đo, không bằng suy đoán.
- `Camera.Rotation`(192) chỉ là 9 float của CFrame (0..35), `Camera.Position` ở 228..239.
  Hỏi: engine có recompute rotation từ `Camera.CFrame` / `Camera.CameraSubject` / `Camera.CameraType`
  mỗi RenderStep không? Giá trị ta ghi **tồn tại bao nhiêu ms**?
  → Viết probe: ghi 1 rotation đã biết, sample mỗi 1 ms trong 200 ms, in thời điểm bị revert.
- Rotation-only có làm lệch field dẫn xuất (lookVector/rightVector/upVector cache) khiến engine
  "không tin" basis của ta? Có cần ghi cả `Camera.Position` / `FieldOfView`?
- Convention: columns `Right, Up, -Look` có đúng không? Kiểm bằng cách ghi basis đã biết rồi so
  `Camera.Rotation` với `VisualEngine.ViewMatrix` (đo bằng ViewMatrix, **không** đo bằng field vừa ghi).
- Ma trận trực giao: sau ~10^4 lần nlerp + normalize, kiểm `R^T R - I`.
- Thí nghiệm bắt buộc: (a) ghi 1 lần; (b) 250 Hz; (c) 1000 Hz; (d) ghi cả 12 float CFrame;
  (e) chỉ ghi khi có mouse input; (f) ghi rồi đợi 1 frame; (g) ghi lúc game 60 vs 240 fps.
- **Deliverable:** bảng write-surface + kết luận field nào authoritative, tần số tối thiểu,
  độ trễ (ms) từ write → đổi hướng render.

### W2. Timing & concurrency
- `writerThread` vs main loop vs game RenderStep: đo jitter thật của gate 4 ms, phân bố `dt`,
  `Sleep(2ms)` thực tế khi CPU bận (game ăn 1 core), `timeBeginPeriod(1)` có tác dụng?
- `g_velLastActNs` là stamp **dùng chung**: mode 0 và mode 1 có thể tranh nhau (kể cả khi mode 0
  chỉ chạy do fallback). Kiểm tra thứ tự tiêu stamp.
- `g_snapEpoch/seq/camAddr` + `fresh` (epoch khớp, camAddr == `g_currentCam`, age < 80 ms):
  có cửa sổ nào `ready=true` mà writer bỏ qua vô hạn (age luôn > 80 ms do main loop chậm)?
- Main loop đọc `vm` **1 lần/frame trước** aim tick; writer ghi rotation **giữa** hai lần đọc →
  `w2s` dùng ma trận cũ. Đo độ lệch (px) khi target di chuyển nhanh ở 60 fps.
- `!wantShow → Sleep(50); continue` (dòng 2510): chứng minh có/không xảy ra khi game mất
  foreground (RMB + alt-tab, bấm vào overlay, console hiện lên). Đây là nghi phạm "camera đứng"
  hoàn toàn hợp lý: aim tick không chạy thì không có write nào.
- **Deliverable:** timeline 1 lần actuation (đọc → tính → ghi → engine xử lý), histogram dt,
  kết luận tranh chấp nào có thật.

### W3. Math & control law
- Với `smoothing=7.5`: `tau=60 ms`, `alpha(4ms)=0.0645`. Kiểm: hội tụ 30° mất bao lâu **đo thật**
  (không dùng `-aimtest`), có overshoot/limit cycle không.
- `dot > 0.99996` (≈0.51°) **và** `distPx < deadzone` (1 px) là **hai** deadzone khác đơn vị.
  Chứng minh không có dải chết ở giữa (0.51° ≈ bao nhiêu px ở FOV 70°/1080p?).
- nlerp gần đối cực (`dot ≈ -1`) + normalize: có nhảy 180° hoặc kẹt không?
- `buildAimMatrix` khi `|ly| > 0.999` (nhìn thẳng lên/xuống) đổi up-vector → có gây roll đột ngột
  (repro: target đi qua đỉnh đầu)?
- Pitch clamp: engine có clamp ±90°? Ta ghi basis vượt clamp thì engine làm gì?
- **Deliverable:** bảng số đo (thời gian hội tụ, sai số còn lại, hành vi ở biên), kết luận
  smoothing có phải nguyên nhân (giả thuyết: **không**, nhưng phải chứng minh).

### W4. Đường input (mode 0) và mouse-lock — dù phiên này không dùng
- `sendRelMouse` + `mouseSvc` (`MouseService + 0x21C`=f), `kVelextSensRvaA/B/C`
  (`base+0x8D857D8/E0/820`) — đây là hardcode duy nhất theo build; kiểm bằng probe.
- `ClipCursor` (`mouseClipRelease/Restore`): `menu.unlockmouse=0` nên hiện tắt. Chứng minh nó
  **không** can thiệp (log `RESTORE`/`RESTORE-failed`).
- `aim.key=RMB` trùng nút xoay camera của Roblox: làm thí nghiệm đổi `aim.key=X` (key không dùng
  trong game) để tách biến. Nếu freeze biến mất → nguyên nhân là tranh chấp input/mouse-lock,
  không phải writer.
- Back-pressure `bp*` (stale-hold 6 lần, probe 750 ms, cursor guard) — chỉ chạy mode 0; xác nhận
  nó không thể chạm vào mode 1 (nếu có, đó là bug).
- **Deliverable:** kết luận mode 0 có/không liên quan + bảng thí nghiệm đổi key.

### W5. Các writer tranh chấp (spectate / stunt / fly / tp / world / hitbox)
Vì "đôi lúc" thường = trạng thái khác bật/tắt:
- `Spectate::publish` chạy **mỗi frame kể cả khi panel ẩn**: nếu `g_on` bị kẹt 1 (falling edge
  không tới) thì nó ghi `Camera.CameraSubject` = Humanoid của người khác/xác chết → camera bám
  chỗ khác, mouse look vô hiệu. Kiểm: log trạng thái `g_on/g_wasOn` + test bật/tắt spectate.
- `Stunt`: preset `loop=1` ghi CFrame HRP liên tục; `g_on` kẹt 1 thì HRP bị ghim mỗi frame →
  nhân vật đứng tại chỗ, camera không đi theo. Kiểm: log trạng thái stunt + test bật/tắt.
- `Fly`: writer 1e6 writes/s giữ velocity; không ghi camera nhưng ăn CPU → ảnh hưởng jitter W2.
- `Tp`: pin/restore `CanCollide`; nếu restore trượt → nhân vật rơi, camera "đứng" theo.
- `World` tab: FOV/zoom ghi liên tục nếu bật (`world.fovon`), zoom `world.zoom` — FOV sai làm
  `w2s`/sensitivity lệch.
- **Deliverable:** bảng "ai ghi gì vào Camera/HRP, tần số, điều kiện bật/tắt" + cách chứng minh
  exclusivity (mỗi lúc chỉ 1 writer sở hữu camera).

### W6. Target validity & cache (ghost target)
- Chứng minh/loại bỏ: target world position đóng băng do prim chết, model husk, bone stale,
  NPC model bị destroy. Đã có `ext_primcheck.exe` + rule `Instance.Parent`; cần thêm:
  - test giết target / target respawn / target teleport / target rời vùng stream;
  - đo `Primitive.Owner` back-pointer theo thời gian (phát hiện prim bị free rồi tái sử dụng);
  - kiểm `Humanoid.Health` lag: health đọc còn 100 nhưng part đã destroy → bao lâu?
- **Deliverable:** bảng trạng thái target → giá trị `Ent` tương ứng → quyết định cache
  (giữ/bỏ/re-resolve), kèm log tag.

### W7. Observability — bắt được lúc freeze thật
Bổ sung instrumentation (tạm thời, có công tắc trong cfg hoặc `-diag2`) để lần freeze sau
để lại bằng chứng **không thể tranh cãi**:
- Mỗi 200 ms khi aim bật: `tgt world`, `dot`, `alpha`, `distPx`, `writes/s`, `vm` hash,
  `Camera.Rotation` sau write, `ViewMatrix` row (để chứng minh engine có đổi hướng không),
  `wantShow`, `fgPid==m.pid`, `g_on` của spectate/stunt/tp.
- **Freeze detector tự động**: nếu target world đổi > 0.05 studs trong 1 s mà `ViewMatrix` không
  đổi > 0.1° → ghi `FREEZE-SUSPECT` kèm snapshot đầy đủ (dump 48 byte Camera + 16 float VM).
- Log hiện có (giữ nguyên, đừng đổi tên): `aim tick`, `writer-deadzone`, `writer-reverted`,
  `writer-disabled`, `aim-no-actuator`, `aim-bone-stale`, `snapshot-publish`, `camera changed`,
  `entity re-resolve`, `entity refresh`.
- **Deliverable:** bộ log tag cuối cùng + hướng dẫn đọc 20 dòng quanh `FREEZE-SUSPECT`.

### W8. Harness & acceptance
- Script kiểm tự động (PowerShell) chạy W0 + parse log → PASS/FAIL.
- Kịch bản acceptance: giữ aim 60 s trên target di chuyển, ở 60/144/240 fps; thả key → mouse look
  trả lại **ngay** (<50 ms); 0 `FREEZE-SUSPECT`; 0 `writer-reverted`; độ trễ bám target < 150 ms.
- **Deliverable:** `tools\aim_accept.ps1` + bảng kết quả.

---

## 6. Danh sách giả thuyết phải đóng (mỗi cái: chứng minh hoặc loại bỏ)

| # | Giả thuyết | Cách đo |
|---|---|---|
| H1 | Engine revert rotation ngay (~1 ms) → 250 Hz vẫn thua | W1 probe revert-timing |
| H2 | Basis ta ghi không khớp convention → engine xoay sai/không nhận | W1 so `Camera.Rotation` vs `ViewMatrix` |
| H3 | Thiếu field dẫn xuất (lookVector cache / CFrame position) → engine bỏ qua | W1 (d) ghi 12 float |
| H4 | nlerp + hai deadzone tạo dải chết (0.51° vs 1 px) | W3 |
| H5 | `wantShow=false` (mất foreground / panel ẩn) → aim tick không chạy | W2 + log `wantShow` |
| H6 | Spectate/stunt/tp ghi đè Camera/HRP (state kẹt) | W5 |
| H7 | `vm` cũ 1 frame → publish nhắm điểm ảo, `dot` kẹt | W2 |
| H8 | Ghost target (prim chết, world đóng băng) | W6 + `aim tick world` |
| H9 | RMB trùng nút xoay camera Roblox / mouse-lock state | W4 (đổi `aim.key`) |
| H10 | Writer thread starvation khi game ăn CPU | W2 histogram |
| H11 | Mode 2 viewport shift còn sót (`Camera.Viewport` bị ghi) | grep + log Viewport |
| H12 | Ma trận trôi khỏi trực giao sau nhiều lần nlerp | W1 (f) |

---

## 7. Format findings (bắt buộc)

Tạo `FINDINGS_aim_camera_re.md` với đúng cấu trúc:

```md
# FINDINGS — aim camera rotation (phiên RE <ngày>)

## 0. Baseline & repro
- exe sha256, offsets hash/version, cfg snapshot
- bảng output các gate (aimtest/offtest/srvtest/verify/primcheck)
- định nghĩa FREEZE đo được + kịch bản

## 1. Bảng bằng chứng
| # | đo cái gì | cách đo | số liệu | kết luận | độ tin cậy |
|---|---|---|---|---|---|
(chỉ ghi cái ĐO ĐƯỢC; cái suy luận ghi rõ "suy luận")

## 2. Nguyên nhân xếp hạng
| hạng | giả thuyết | bằng chứng ủng hộ | bằng chứng phủ định | trạng thái |
|---|---|---|---|---|

## 3. Patch
- file:line, diff, giải thích cơ chế, vì sao không phá mode 0/2

## 4. Đo trước/sau
- cùng kịch bản W8, bảng số

## 5. Câu hỏi còn mở / rủi ro
```

Quy tắc ghi: mọi khẳng định phải có `file:line` hoặc số đo. Cấm câu "có vẻ", "có thể là"
mà không kèm dữ liệu. Nếu bí, ghi rõ "CHƯA BIẾT" + thí nghiệm đề xuất — **không đoán**.

---

## 8. Gate không được phá (chạy sau mỗi thay đổi)

```
build_fast.bat                       -> exit 0, in sha256
ext_overlay.exe -aimtest             -> exit 0, 522 dòng, 0 FAIL
ext_overlay.exe -offtest offsets_fixtures        -> keys=58 fails=0
ext_overlay.exe -offtest offsets_fixtures\negative -> FAIL + exit 1
ext_overlay.exe -srvtest 0           -> === done (PASS)
build.bat                            -> toàn bộ tool exit 0
ext_overlay.exe -verify              -> verify: PASS (cần game mở)
ext_primcheck.exe                    -> PRIMCHECK PASS (cần game mở)
ext_overlay.exe -diag -nohide -t 4   -> hit counts 8 tab không đổi bất thường
src\overlay.cpp + src\ovl\*          -> LF-only, không BOM, không CR
overlay.cfg                          -> key set không đổi nếu chỉ refactor
```

---

## 9. Luật chơi (đọc kỹ, đừng vi phạm)

1. **Chỉ 1 tool aim một lúc.** `ext_overlay.exe` giữ mutex `ext_overlay_singleton`; instance 2 tự
   thoát. Người dùng sẽ chạy `velext_v1.exe` — **không** chạy song song với overlay để test aim.
   `taskkill /IM ext_overlay.exe /F` trước khi build nếu file bị khoá (build script `exit /b 2`).
2. **Không bịa offset.** Mọi số phải đến từ dump (`offsets_cache.json`) hoặc đo trực tiếp.
   Không thêm fallback "cho chắc": writer ghi 36 byte, sai offset là phá camera. Dùng mọi công cụ có thể để xử lý reverse engineering velext_v1.exe
3. **Không ghi `Primitive.Position`** cho fly/teleport (engine revert trong 10–50 ms). Fly/TP là
   velocity writer tần số cao — xem `HANDOFF_fly_velocity.md`.
4. **Không bỏ gate.** `validateOffsets()` là cổng cứng; version mismatch = read-only.
5. **Không mở feature mới, không đổi key cfg, không đổi tên log tag** — log là bằng chứng dùng
   chung giữa các phiên.
6. Mọi thay đổi phải chứng minh không phá mode 0 và mode 2 (dù người dùng không dùng).
7. Sandbox để test UI/diag: copy exe + `offsets_cache.json` + `overlay.cfg` ra thư mục riêng
   (cfg/offsets resolve **cạnh exe**, không phải CWD).
8. Đừng kết luận "đã sửa" khi chưa có kịch bản W8 PASS. Người dùng đã bị hứa 3 lần rồi.

---

## 10. Định nghĩa "đã sửa xong"
-Nguoief dùng test thử trong game và không còn gặp lỗi nữa
- W8 PASS: 60 s aim trên target di chuyển, 3 mức fps, **0** `FREEZE-SUSPECT`,
  **0** `writer-reverted`, thả key → mouse look trả lại < 50 ms.
- `FINDINGS_aim_camera_re.md` có nguyên nhân gốc **kèm bằng chứng đo**, không phải giả thuyết.
- Patch tối thiểu, có giải thích cơ chế engine (vì sao cách mới thắng được engine).
- Gate §8 xanh toàn bộ.
