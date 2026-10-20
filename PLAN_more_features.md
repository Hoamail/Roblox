# PLAN — mở rộng `ext_overlay`: danh sách feature mới

Ngày: 2026-10-17
Trạng thái: **draft để user chọn hướng** (chưa code gì)
Build đang chạy: `version-11f155f3450f4c8e`, place test `101770480176177`
Nguồn offset: `offsets.imtheo.lol` (392 offset, pinned theo version)

---

## 0. Khung & giới hạn (đọc trước khi mong đợi feature nào đó)

### 0.1 External làm được gì / không làm được gì

`ext_overlay` là **external thật**: chỉ `ReadProcessMemory` (qua syscall trực tiếp) + `NtWriteVirtualMemory`,
**không inject DLL, không hook, không chạy Lua trong game**. Hệ quả cứng:

| Không làm được | Vì sao |
|---|---|
| Silent aim / aimbot kiểu "bullet bending" | Phải hook `Raycast`/`FireWeapon`/gọi lua — cần injection |
| Chams thật, Glow, Outline, wireframe mesh | Phải hook pipeline render (D3D11 của game) |
| Wallbang (bắn xuyên tường) | Server-side |
| Anti-kick (chặn `Kick()` / chặn report) | Cần hook `ScriptContext`/remote — injection |
| Executor / chạy script / DEX full | Cần Lua VM trong process |
| Đọc/chặn remote traffic, "desync/ghost mode" | Cần hook replication |
| Godmode thật (HP không giảm cho server) | Server authoritative |

Còn lại — **mọi thứ chỉ cần đọc memory game và vẽ/về logic của riêng mình** — đều làm được, và đó là
phần lớn danh sách dưới đây. Chams/glow khi cần thì làm **bản gần đúng** (box tô mờ, silhouette 2D).

### 0.2 Nguyên tắc an toàn khi ghi memory (đã trả giá bằng 268)

- 268 = Hyperion **client-side kick**, không ban, rejoin được. Nhưng mỗi lần ghi là một cơ hội để nó thấy.
- Luật: **chỉ ghi khi feature được tick + chỉ ghi đúng field cần + rate-limit + restore khi tắt**.
- Không ghi mỗi frame vào nhiều field cùng lúc. Ghi theo "diff" (giá trị đổi mới ghi).
- Field nào local-only (Lighting, Atmosphere, Camera, Terrain, Gui) = **an toàn nhất** (chỉ đổi render máy mình).
- Field nào replicate lên server (Position, Velocity, Rotation, Health, WalkSpeed) = **rủi ro trung bình**.
- Mọi write mới đi qua **một hàm trung tâm** `safeWrite(addr, val, kind)` có: whitelist offset, đếm số write/s,
  log diag, và tự tắt feature khi phát hiện 268 (gameunload → entity count = 0 trong khi process còn sống).
- Thêm **panic key** (một phím): tắt hết feature + ẩn menu + ẩn window khỏi screen capture.

### 0.3 Protocol test (giữ nguyên như cũ, không đổi)

1. Account phụ + **place riêng** (không test ở place chính).
2. **Không chạy overlay mình và velext cùng lúc.**
3. Mỗi feature mới: bật → quan sát trong game → tắt → xem có side effect (kẹt camera/kẹt clip/mất char) không.
4. Test click/menu phải dùng `-nohide` và toạ độ lấy từ log `-diag` (`hit[i] = l,t - r,b`).
5. Đọc Roblox (RPM read-only) thì được phép bất cứ lúc nào; **ghi** thì theo mục 0.2.

### 0.4 Convention

- Sau mỗi đợt lớn: viết handoff `<ten>_handoff.md` + backup `backup/overlay_<TEN>_<ngày>.{cpp,cfg}` như đã làm.
- Mỗi feature: có dòng log diag riêng khi chạy `-diag` (ví dụ `vis fogEnd=... sky=...`).

---

## 1. Offset đã có sẵn mà chưa dùng (kho vàng cho feature mới)

Đọc từ `offsets.imtheo.lol/offsets.json` (đúng version đang chơi). Không cần RE thêm cho các mục này:

| Namespace | Field (offset) | Mở ra feature |
|---|---|---|
| Camera | Position 236, Rotation 200 (matrix 9 float), CameraSubject 184, FieldOfView 304, CameraType 296, ViewportSize 716 | FOV changer, zoom xa, freecam, camera lock, spin camera |
| Lighting | Brightness 264, Ambient 192, OutdoorAmbient 240, FogStart 288, FogEnd 284, FogColor 228, ClockTime 184, GlobalShadows 308, ExposureCompensation 276, EnvironmentDiffuse/Specular 268/272 | Fullbright, anti-fog, tắt bóng, đổi giờ, exposure, custom màu |
| Atmosphere | Density 192, Haze 200, Glare 196, Color 168, Decay 180 | Xoá sương mù, nhìn xa |
| Bloom/Blur/ColorCorrection | Enabled 160 (+Intensity/Size/Brightness/Contrast/Tint) | Tắt bloom/blur/CC → nét + FPS |
| Terrain | WaterTransparency 492, WaterReflectance 488, WaterWaveSize 496, GrassLength 480, WaterColor 464 | Nhìn xuyên nước, nước phẳng, ít cỏ |
| World (Workspace→World=1024) | Gravity 544, FallenPartsDestroyHeight 532, AirProperties 568, Primitives 712 | Đổi trọng lực (moon jump/zero-G), chống rơi void, wind |
| Player | AccountAge 860, UserId 192, MinZoomDistance 876, MaxZoomDistance 872, CameraMode 880 | Info player, zoom siêu xa, đổi camera mode |
| PlayerMouse/MouseService | MousePosition 196, SensitivityPointer 0 | Đọc chuột + sensitivity của game (calib aim) |
| Humanoid | HipHeight 400, MaxSlopeAngle 424, Jump 471, Sit 474, PlatformStand 473, AutoJumpEnabled 464, AutoRotate 465, EvaluateStateMachine 468, BreakJointsOnDeath 213, FloorMaterial 384, HumanoidStateID 32, SeatPart 248, MoveDirection 316 | Infinite jump, auto-jump, float/hover, hip height, trượt dốc, instant respawn, biết đang đứng trên gì |
| Primitive | Rotation 176, Size 452, Flags 446, Owner 536, AssemblyLinearVelocity 224, AssemblyAngularVelocity 236, Material 0 | Spinbot, anti-fling, fling, đọc vật liệu đang đứng |
| BasePart | Transparency 288, Color3 57, CastShadow 296, Reflectance 252, Shape 424 | Lọc item ESP (bỏ part vô hình), cham giả, tắt bóng part |
| Model | PrimaryPart 584, Scale 308 | ESP theo model, đọc scale char (to/nhỏ bất thường) |
| DataModel | JobId 272, ServerIP 1464, PlaceVersion 420, CreatorId 376, GameLoaded 1488, PrimitiveCount 1048 | Server info, server hop, auto-rejoin, đếm part |
| ProximityPrompt / ClickDetector | ObjectText 192, ActionText 160, MaxActivationDistance 280, Enabled 294 | ESP điểm tương tác (chest/collect/két) |
| Sound | SoundId 184, IsPlaying 304, RollOffMaxDistance 272 | Sound ESP (tiếng bước chân, tiếng súng) |
| Animator / AnimationTrack | ActiveAnimations 2688, Animation 168, Speed 196, TimePosition 200 | Biết đối thủ đang chơi animation gì (đang vung vũ khí → cảnh báo) |
| StatsItem | Value 1280 (+ Stats service) | **Ping thật** của game |
| Attachment | Position 180 | Lấy toạ độ móc (bone chính xác hơn cho aim/skeleton) |
| World/FallenPartsDestroyHeight 532 | | Anti-void / cảnh báo sắp rơi |

Thiếu offset (muốn có thì phải RE thêm, hoặc dùng giá trị gần đúng): `Camera.Focus`, `Camera.ViewportSize` có rồi,
`Sky.SkyboxBk..Up` có nhưng cần **ghi string** (khó, xem 6.3), `Ping` riêng lẻ.

---

## 2. Đề xuất cấu trúc menu

Hiện tại 4 tab (Body / Combat / Teleport / Misc) đã chật. Đề xuất 2 hàng tab, 8 tab:

```
[ Body ] [ Combat ] [ Visual ] [ World ]
[ Teleport ] [ Server ] [ Tools ] [ Misc ]
```
- **Body**: speed, jump, fly, noclip, infinite jump, hip height, gravity, spinbot, anti-fling, freecam.
- **Combat**: aim, trigger, autoclick, hitbox + aim v2 knobs + hitmarker.
- **Visual**: ESP element toggles + màu + skeleton/healthbar/snapline/OOF + radar + item ESP + FOV.
- **World**: lighting/fog/time/fullbright + atmosphere + water + effects off + skybox.
- **Teleport**: player list (có avatar/account age), waypoint, TP toạ độ, click-TP.
- **Server**: place/jobId/IP/region/ping/uptime, hop, rejoin, auto-rejoin, anti-AFK, multi-instance.
- **Tools**: instance explorer (DEX-lite), saveinstance-lite, macro recorder, memory scan (tool rời).
- **Misc**: keybind editor, profiles, watermark, streamproof, panic, debug.

Vấn đề kỹ thuật cần giải: **scroll trong tab** (tab World/Tools sẽ dài hơn panel 380px) → dùng `BeginChild`
có thanh cuộn (đã có hạ tầng `g_menuHits` cho child, đang dùng ở tab Teleport) + ghi `menu.size` khi resize.

---

## 3. Lộ trình theo đợt

Ước lượng: **S** = 1 buổi, **M** = 1–2 buổi, **L** = 3+ buổi. "Giá trị" 1–5. "Rủi ro" = nguy cơ 268/bị phát hiện.

### ĐỢT 1 — Quick wins, ít rủi ro, thấy ngay (đề xuất bắt đầu ở đây)

| # | Feature | Cách làm | Công | Giá trị | Rủi ro |
|---|---|---|---|---|---|
| 1.1 | Keybind editor trong menu | Click vào label key → "press any key" → lưu cfg. Refactor `keyStateLabel` thành `keyBind()` | M | 5 | 0 |
| 1.2 | Profiles (config theo tên + auto theo placeId) | `profiles/<name>.cfg`, nút Save/Load/Delete, auto-load khi `placeId` đổi (đã có `lastPlaceSeen`) | M | 5 | 0 |
| 1.3 | Watermark + status HUD | ImGui window nhỏ: fps, ping, place, players, feature đang bật, loop ms | S | 3 | 0 |
| 1.4 | ESP elements v2 | HP bar, distance, team color, head dot, snapline, off-screen arrow, **skeleton** (đã có `headPrim` + `limbPrim[4]`), render distance, bật/tắt từng element + ColorEdit4 | M | 5 | 0 (chỉ vẽ) |
| 1.5 | Radar 2D | Góc màn hình, dot theo self + yaw (lấy từ `Camera.Rotation`) | S | 4 | 0 |
| 1.6 | Waypoint | Save/name/list/xoá/TP (dùng lại khối teleport), lưu vào cfg | S | 4 | 0 |
| 1.7 | Streamproof + panic key | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`, `WS_EX_TOOLWINDOW`, panic = tắt hết + ẩn | S | 3 | 0 |
| 1.8 | Tab scroll + resize an toàn | `BeginChild` cuộn cho tab dài | S | 4 | 0 |

### ĐỢT 2 — Visual & World control (local-only → gần như không rủi ro, thấy rõ nhất)

| # | Feature | Cách làm | Công | Giá trị | Rủi ro |
|---|---|---|---|---|---|
| 2.1 | Fullbright | `Lighting.Brightness=3`, `Ambient/OutdoorAmbient=200` (slider) | S | 5 | 0 |
| 2.2 | Anti-fog | `FogStart=1e5`, `FogEnd=1e5` (+ restore) | S | 5 | 0 |
| 2.3 | No shadows / exposure / time | `GlobalShadows=0`, `ClockTime` slider, `ExposureCompensation`, `EnvironmentDiffuse/Specular` | S | 4 | 0 |
| 2.4 | Tắt bloom/blur/CC | Walk child lớp `BloomEffect`/`BlurEffect`/`ColorCorrectionEffect` của Lighting+Camera → `Enabled=0` | S | 4 | 0 |
| 2.5 | Xoá Atmosphere | `Density=0`, `Haze=0`, `Glare=0` | S | 3 | 0 |
| 2.6 | Nước trong + nước phẳng | `Terrain.WaterTransparency=1`, `WaterWaveSize=0`, `WaterReflectance=0` | S | 4 | 0 |
| 2.7 | Xuyên cỏ + đổi màu nước | `GrassLength=0`, `WaterColor` | S | 2 | 0 |
| 2.8 | FOV changer + zoom xa | `Camera.FieldOfView` (ghi mỗi frame khi bật), `Player.Min/MaxZoomDistance=0.5/2000` | S | 4 | 0 |
| 2.9 | Item / Object ESP | Walk `Workspace` (depth + node cap, refresh 500ms), lọc theo danh sách tên (từ Explorer), vẽ box+tên+khoảng cách; bỏ part `Transparency=1` | L | 5 | 0 (đọc) |
| 2.10 | ProximityPrompt ESP | Walk tìm `ProximityPrompt`/`ClickDetector`, vẽ `ObjectText/ActionText` tại vị trí | M | 4 | 0 |
| 2.11 | Sound ESP (tuỳ chọn) | Tìm `Sound` đang `IsPlaying` trong bán kính → hiện điểm phát + tên | M | 2 | 0 |
| 2.12 | Cham giả (approximation) | Box tô mờ 40% + viền đậm (không thể là chams thật) | S | 2 | 0 |

### ĐỢT 3 — Body/Movement nâng cao (có write → rủi ro trung bình, cần test kỹ)

| # | Feature | Cách làm | Công | Giá trị | Rủi ro |
|---|---|---|---|---|---|
| 3.1 | Noclip | Mỗi frame clear bit CanCollide trong `Primitive.Flags` các part của mình (hàm RMW đã có ở `hitboxApplyPart`), restore khi tắt | S | 5 | thấp |
| 3.2 | Infinite jump / auto-jump | Ghi `Humanoid.Jump=1` (471) khi giữ phím, hoặc `AutoJumpEnabled=1` | S | 4 | thấp |
| 3.3 | Hip height / float | `Humanoid.HipHeight` (400) slider + `PlatformStand` (473) cho hover | S | 3 | thấp |
| 3.4 | Gravity slider | `Workspace→World.Gravity=544` (0..196) → moon jump / zero-G | S | 4 | thấp |
| 3.5 | Max slope + trượt tốt hơn | `Humanoid.MaxSlopeAngle=424` | S | 2 | thấp |
| 3.6 | Anti-fling | Giám sát `AssemblyLinearVelocity` của mình; vượt ngưỡng → zero + restore rotation | M | 4 | thấp |
| 3.7 | Anti-void | `World.FallenPartsDestroyHeight` xuống -5000; hoặc watchdog: nếu Y < ngưỡng → TP về waypoint an toàn | S | 4 | thấp |
| 3.8 | Instant respawn / force reset | Ghi `Humanoid.Health=0` (hoặc `BreakJointsOnDeath`) | S | 3 | thấp |
| 3.9 | Sit / stand / platform stand | `Sit` (474), `PlatformStand` (473), `EvaluateStateMachine` | S | 2 | thấp |
| 3.10 | Spinbot | Ghi `Primitive.Rotation` matrix theo góc xoay mỗi frame (tốc độ + trục tuỳ chọn) | M | 3 | trung |
| 3.11 | Freecam | `Camera.CameraType=Scriptable` + ghi `Position`/`Rotation` mỗi frame; WASD+Space/Ctrl+Shift speed; **restore CameraType khi tắt/thoát** (rủi ro: `Camera.Focus` không có offset → cần set cờ restore cẩn thận) | L | 5 | trung |
| 3.12 | Click-TP / TP toạ độ / TP tới item | TP tới người dưới con trỏ, tới X,Y,Z nhập tay, tới item gần nhất | M | 5 | trung |
| 3.13 | Fly v2 | Thêm mode camera-relative, lên/xuống, boost, fly-to-waypoint | M | 4 | trung |
| 3.14 | Fling (ném người khác) | Ghi velocity lớn vào root part mình khi va chạm | M | 3 | **cao** (bị report, physics server) |

### ĐỢT 4 — Server / account / tool rời (không đụng memory game → an toàn)

| # | Feature | Cách làm | Công | Giá trị | Rủi ro |
|---|---|---|---|---|---|
| 4.1 | Server info panel | `DataModel.JobId` (272), `PlaceId`, `PlaceVersion`, `ServerIP` (1464) + region (HTTP, mặc định TẮT), uptime, players/max | S | 4 | 0 |
| 4.2 | Ping thật | Walk `Stats` service → `StatsItem.Value` (1280) tên "Data Ping" | M | 3 | 0 |
| 4.3 | Server hop | `GET games.roblox.com/v1/games/{placeId}/servers/Public?sortOrder=Asc&excludeFullGames=true` → chọn theo filter (min players, region, loại server hiện tại) → mở `roblox://experiences/start?placeId=X&gameInstanceId=Y` | M | 5 | 0 |
| 4.4 | Rejoin / join theo jobId / join bạn bè | Cùng cơ chế URI + `roblox-player:` auth ticket nếu cần | M | 4 | 0 |
| 4.5 | Auto-rejoin khi 268 / disconnect | Watchdog: process còn sống nhưng `GameLoaded=0`/hết entity >5s → relaunch URI với jobId đã lưu | M | 5 | 0 |
| 4.6 | Anti-AFK | `SendInput` phím/chuột nhẹ mỗi 10–15 phút (khi game foreground); fallback: nudge camera | S | 4 | 0 |
| 4.7 | Player info trong tab Teleport | `AccountAge` (860), `UserId`, avatar headshot (thumbnails API), team, hp, khoảng cách, log "user đã gặp" | M | 4 | 0 |
| 4.8 | **ext_multi.exe** (multi-instance) | Tool riêng: `CreateMutexW(bInitialOwner=TRUE, "ROBLOX_singletonMutex")` + `ROBLOX_singletonEvent` giữ trên thread riêng, **chạy trước Roblox**, không cần admin. Tray icon + "kill all clients" | M | 5 | 0 |
| 4.9 | **ext_fps.exe** / tab Perf | Sửa `<int name="FramerateCap">` trong `%LOCALAPPDATA%\Roblox\GlobalBasicSettings_13.xml` (phải làm khi Roblox đã tắt — hiện file user đang là 30!). Kèm preset tắt cỏ/quality. Lưu ý: allowlist FastFlag 9/2025 → `ClientAppSettings.json` gần như vô dụng, đường XML mới đúng | M | 4 | 0 |
| 4.10 | Notification join/leave | Diff entity list → toast "X vào server / Y rời" | S | 3 | 0 |

### ĐỢT 5 — Combat v2

| # | Feature | Cách làm | Công | Giá trị | Rủi ro |
|---|---|---|---|---|---|
| 5.1 | Aim v2 knobs | Sticky delay, auto-switch khi target chết, humanize (noise/jitter), smoothing từng trục, easing (linear/exp/bezier), aim part picker UI (Head/Torso/limb — có sẵn `limbPrim`), FOV style/fill, "target line", air-target override, update-rate riêng | M | 5 | thấp |
| 5.2 | Trigger v2 | min/max delay randomization, burst fire (count+delay), auto-release, part/hitbox scale riêng, headshot-only | M | 4 | thấp |
| 5.3 | Prediction v2 | Dùng `AssemblyLinearVelocity` + `AssemblyAngularVelocity`, dự đoán theo ping | M | 4 | thấp |
| 5.4 | Hitmarker + hit sound + kill notify | Theo dõi HP của target đang aim: HP giảm → hitmarker (vẽ) + sound (`PlaySound` của Win, không cần file game) | S | 4 | 0 |
| 5.5 | Game modules framework | Mỗi game 1 file profile: danh sách tên item, part đặc biệt, toạ độ farm, ngưỡng riêng. Ví dụ đầu tiên: auto-parry kiểu Blade Ball (đọc part bóng + velocity → canh click đúng lúc) | L | 4 | trung |
| 5.6 | Aim assist cho NPC/mob | Đã có một phần (`aim.npcs`); thêm mob ESP + ưu tiên theo HP/mob type | S | 3 | thấp |

### ĐỢT 6 — Nặng / chỉ làm khi cần (L)

| # | Feature | Cách làm | Công | Giá trị | Rủi ro |
|---|---|---|---|---|---|
| 6.1 | Wall check (aim/trigger/ESP) | Tự build spatial hash BasePart (Position+Size+Transparency) bán kính ~250 studs, refresh 200–500ms; tự raycast AABB từ camera tới target. Dùng để: chặn bắn xuyên tường, làm mờ ESP mục tiêu bị che | L | 5 | 0 |
| 6.2 | Instance Explorer (DEX-lite) | Cây instance + property viewer read-only trong overlay (đã có `walkChildren`, `dump.cpp`, `explore.cpp` làm nền) | L | 4 | 0 |
| 6.3 | SaveInstance-lite | Dump cây Workspace → JSON/OBJ (recon + làm dữ liệu cho wall check) | L | 3 | 0 |
| 6.4 | Memory scanner tool riêng | `ext_scan.exe`: scan value (int/float/string) → filter → write/freeze (Cheat Engine-lite) → dùng cho game-specific (ammo, coin, timer) | L | 4 | trung |
| 6.5 | Macro recorder / auto-farm generic | Record input + replay theo timeline, loop, nhiều profile | M | 4 | trung |
| 6.6 | Custom skybox / texture | Cần **write TString** (allocate trong memory game) → chỉ làm nếu thật cần | L | 2 | cao |
| 6.7 | Chat tools | Spam/nhắc chat, macro chat bằng SendInput | S | 2 | thấp |
| 6.8 | Animation-based detect | Đọc `Animator.ActiveAnimations` → biết đối thủ đang vung vũ khí/đang block → cảnh báo | M | 3 | 0 |

---

## 4. Đề xuất thứ tự thực hiện (nếu không có ý kiến khác)

1. **ĐỢT 1** (keybind + profiles + ESP v2 + waypoint + streamproof) → biến tool thành "dùng được hằng ngày".
2. **ĐỢT 2** (visual/world) → cảm giác "khác biệt một trời" mà gần như không rủi ro 268.
3. **ĐỢT 3 nhóm write an toàn trước**: noclip, infinite jump, hip height, gravity, anti-void, instant respawn (tất cả đều là ghi 1 field + restore).
4. **ĐỢT 4** (server hop + auto-rejoin + multi-instance) → giá trị dùng thật cao, độc lập với memory.
5. Rồi mới tới **freecam / spinbot / fling** (rủi ro trung-cao, cần test riêng).
6. **ĐỢT 5, 6** theo nhu cầu game cụ thể.

---

## 5. Ghi chú kỹ thuật đáng nhớ khi bắt tay

- **`hitboxApplyPart` (overlay.cpp ~803–830) đã làm read-modify-write `Primitive.Flags`** → noclip chỉ cần tái dùng pattern này, rất rẻ.
- **Skeleton ESP gần như free**: `Ent` đã có `headPrim` + `limbPrim[4]` (LeftHand/RightHand/LeftFoot/RightFoot) → chỉ cần `w2s` + `AddLine`.
- **`lastPlaceSeen` (overlay.cpp ~1446)** đã có → auto-load profile theo place chỉ là thêm nhánh.
- **Auto-rejoin**: `JobId` (272) + `roblox://experiences/start?placeId=..&gameInstanceId=..` (đã verify từ tài liệu protocol roblox) → dùng lại đúng URI khi bị 268.
- **FPS cap**: file `GlobalBasicSettings_13.xml` của user hiện đang `FramerateCap=30` → feature 4.9 có giá trị thật ngay. Phải sửa khi Roblox đã đóng, không thì client ghi đè lúc thoát.
- **Freecam**: `CameraType` enum Roblox: Fixed=0, Attach=1, Watch=2, Track=3, Follow=4, Custom=5, Scriptable=6, Orbital=7. Khi set Scriptable, game không đụng camera nữa → ta tự ghi Position/Rotation. **Bắt buộc restore** CameraType khi tắt feature / thoát overlay (thêm vào danh sách restore cùng `flyAnchorRestore`, `hitboxRestoreAll`, `mouseClipRestore`).
- **Lighting/Atmosphere/Terrain/Bloom**: tìm instance bằng `sdk.findChildByClass(dm, "Lighting")`, `Workspace`→`Terrain`, effect là child của Lighting (đôi khi nằm trong Camera) → walk cả hai, cache pointer.
- **Item ESP**: phải có node cap + depth cap + cache name (đã có `classCache`); đo `loopMs` trước/sau để chắc không tụt FPS.
- **Write string** (skybox, chat) là hạn chế thật: cần allocate trong heap của game → để cuối, không hứa.

## 6. Những thứ KHÔNG đưa vào plan (và lý do)

1. Silent aim / wallbang / anti-kick / hook remote — cần injection, ngoài kiến trúc external.
2. Chams/glow thật, custom render — cần hook D3D11 của game.
3. Godmode, "kill all", "damage aura" — server-side, chỉ là ảo giác client.
4. Bypass Hyperion / anti-cheat evasion chuyên sâu (driver kernel) — ngoài phạm vi và làm tăng rủi ro không cần thiết khi mục tiêu là chơi/experiment trên place test.
