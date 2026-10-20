# PROMPT — BUILD tab World (ext_overlay): Đợt 2 "Visual & World control"

Bạn là coding agent trên repo `C:\Users\adminn\ext_project` (Windows / PowerShell / mingw g++).
`ext_overlay.exe` là **external overlay** (ImGui + DX11, `src\overlay.cpp`): chỉ RPM +
NtWriteVirtualMemory, **không inject DLL, không hook, không chạy Lua trong game**. Menu đã có 8
tab; **tab World (index 4) hiện chỉ là placeholder**:

```cpp
} else if (cfgMenuTab == 4) {
    // ---------------- World: lighting / atmosphere / terrain -------
    ImGui::BeginChild("##world", ImVec2(0.0f, 330.0f), true);
    markChildArea();
    ImGui::TextDisabled("Fullbright, anti-fog, no shadows, water");
    ImGui::TextDisabled("transparency, FOV/zoom: Đợt 2.");
    ImGui::EndChild();
}
```

Việc hôm nay: **hiện thực đầy đủ tab này theo PLAN Đợt 2 (mục 2.1–2.8) — offline, không cần
chạy Roblox.**

## Đọc trước (bắt buộc, theo thứ tự)

1. `AGENTS.md` — luật project.
2. `PLAN_more_features.md` — **§0.2** (luật ghi memory + 268), **§1** (bảng offset có sẵn),
   **§2** (cấu trúc 8 tab + lưu ý scroll), **§5** (cách tìm Lighting/Terrain/effects, restore).
3. `menu_tabs_handoff.md` — pattern tab / `BeginChild` / `markChildArea` / `hitMark`, lý do dùng
   button thay `BeginTabBar`.
4. `src\overlay.cpp` — placeholder World; helper `chkInt` / `sliderFloatCfg` / `sliderIntCfg` /
   `upsertCfgKey` / `hitMark`; `loadAimCfg()` + khối clamp; các restore mẫu `hitboxRestoreAll`,
   `flyAnchorRestore`, `mouseClipRestore`; khối panic; khối exit.
5. `src\offsets.h` + `src\offsets.cpp` — `Offsets` struct + `OffsetManager::fill()` (phải mở rộng).
6. `src\sdk.h` — `findChildByClass`, `walkChildren`, `readClass`.

## Việc cần làm

### 1) Offsets mới (offline, không cần RE)

Thêm field vào `Offsets` (`offsets.h`) + parse trong `fill()` (`offsets.cpp`). Dữ liệu đã có
trong `offsets_cache.json` hiện tại (và trên offsets.imtheo.lol):

| nhóm | key (offset) |
|---|---|
| Lighting | ClockTime 184, Brightness 264, Ambient 192, OutdoorAmbient 240, EnvironmentDiffuseScale 268, EnvironmentSpecularScale 272, ExposureCompensation 276, FogStart 288, FogEnd 284, FogColor 228, GlobalShadows 308 |
| Atmosphere | Density 192, Haze 200, Glare 196, Color 168, Decay 180 |
| Effects | BloomEffect / BlurEffect / ColorCorrectionEffect: Enabled 160 (+ Intensity/Size/Brightness/Contrast/Tint nếu tiện) |
| Terrain | GrassLength 480, WaterReflectance 488, WaterTransparency 492, WaterWaveSize 496, WaterWaveSpeed 500, WaterColor 464 |
| Camera | FieldOfView 304 (đã có `o.cameraFOV`) |
| Player | MinZoomDistance 876, MaxZoomDistance 872 |

Quy ước: key thiếu → field = 0 → feature tự no-op (không crash). **Không sửa `offsets_cache.json`
bằng tay** — chỉ đọc.

### 2) Resolve instance (runtime, cache)

- `Lighting` = `sdk.findChildByClass(dm, "Lighting", ...)`; `Terrain` = `findChildByClass(ws, "Terrain", ...)`.
- `Atmosphere` + các effects = children của `Lighting` (đôi khi nằm trong `Camera`) → `walkChildren`
  + `readClass`.
- Cache pointer; retry ≤ 1/s khi miss; reset khi place đổi (mirror chỗ `hitboxRestoreAll` được gọi)
  và khi entity cache reset.
- Không tìm thấy → dòng trạng thái "Lighting: not found", mọi toggle no-op + log diag 1 dòng.

### 3) Feature (đúng danh sách placeholder + PLAN 2.1–2.8)

Mỗi mục: cfg key riêng + checkbox trong tab + lưu/restore giá trị gốc.

1. **Fullbright** — Brightness slider 1..3; Ambient & OutdoorAmbient = trắng (3 float r,g,b @192/240).
2. **Anti-fog** — FogStart = FogEnd = 100000 (restore cả 2).
3. **No shadows** — GlobalShadows = 0.
4. **Exposure** — ExposureCompensation slider -2..2.
5. **Time** — ClockTime slider 0..24 (chỉ ghi khi tick; ghi lại khi slider đổi).
6. **Tắt effects** — Bloom / Blur / ColorCorrection → Enabled = 0 (restore từng cái đã đổi).
7. **Atmosphere off** — Density = Haze = Glare = 0 (restore).
8. **Nước trong/phẳng** — WaterTransparency = 1, WaterWaveSize = 0, WaterReflectance = 0 (restore).
9. **Cỏ / màu nước** (tuỳ chọn) — GrassLength = 0; WaterColor (Color3).
10. **FOV + zoom** — FOV slider 70..120, ghi `Camera.FieldOfView` **mỗi frame khi bật** (game reset FOV
    mỗi frame), restore giá trị gốc khi tắt; zoom xa: MinZoomDistance = 0.5, MaxZoomDistance = 2000
    (restore).

Không làm trong đợt này: Gravity (đợt 3), skybox/write string, item ESP.

### 4) Luật ghi memory (§0.2) — BẮT BUỘC

- Chỉ ghi khi feature tick. Apply 1 lần khi bật; sau đó **diff-write**: đọc lại ≤ 4 lần/s, chỉ ghi
  khi giá trị bị game sửa (FOV là ngoại lệ: mỗi frame).
- Lưu original trước khi ghi lần đầu; **restore đầy đủ** khi untick, khi panic, và khi exit — thêm
  `worldRestoreAll(m, o)` cạnh `hitboxRestoreAll(m, o)` ở **tất cả** các chỗ restore hiện có
  (`grep -n "hitboxRestoreAll" src\overlay.cpp`).
- Rate-limit log (`world ...`), chỉ in khi `-diag`.
- Mọi file mới (nếu có) dùng `exePath("...")` — side-file resolution đã chuyển về cạnh exe
  (2026-10-19), **không dùng path tương đối**.

### 5) UI (tab World)

- Thay placeholder trong `} else if (cfgMenuTab == 4) {` bằng các dòng điều khiển; giữ
  `BeginChild("##world", ImVec2(0, ...), true)` + `markChildArea()`.
- Pattern mỗi dòng (theo tab Body): `chkInt("Fullbright", &cfgWorldFull)` → `upsertCfgKey("world.full", ...)`;
  `hitMark(); ImGui::SameLine();` `sliderFloatCfg(...)`; **`hitMark()` sau MỌI widget** (thiếu là click
  xuyên xuống game).
- Child cao ~330 (đủ ~10 dòng); thêm dòng trạng thái: `Lighting 0x... / not found`, `Terrain ...`.
- cfg mới: `world.full`, `world.bright`, `world.fog`, `world.shadows`, `world.expo`, `world.clock`,
  `world.fx`, `world.atmo`, `world.water`, `world.grass`, `world.fovon`, `world.fov`, `world.zoom`.
  Parse trong `loadAimCfg()` + clamp khoảng hợp lệ.

### 6) Verify

- Build: `build_fast.bat` (đóng `ext_overlay.exe` trước — file bị lock).
- `ext_overlay.exe -aimtest` → exit 0.
- Offline: exe không được crash khi chưa resolve được Lighting / không có game (mọi write phải
  null-guard).
- In-game checklist (user chạy): bật từng mục → thấy hiệu ứng; tắt → restore đúng; exit overlay
  khi đang bật → restore; `-diag` có line `world ...`; không có dấu hiệu 268.

### 7) Docs + backup

- Backup trước khi sửa: `backup\overlay_WORLD_<yyyyMMdd>.{cpp,exe}`.
- Viết `HANDOFF_world_tab.md`: việc đã làm, cfg key mới, output self-test, các điểm còn `[I]`/fallback.

## Ràng buộc

- Không đụng logic aim/ESP/body/teleport; chỉ thêm key mới vào parser + clamp + tab World.
- Không refactor lan man; giữ style comment hiện có (`[P]/[I]`, log prefix).
- Không chạy Roblox/velext trong lúc build/self-test offline.
- Không write string / không remote / không hook. Giữ đúng kiến trúc external.

## Build (copy từ build_fast.bat)

```
g++ -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp lib\libimgui.a -Ithirdparty\imgui-master -Ithirdparty\imgui-master\backends -Isrc -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
```

## Báo cáo cuối

Tóm tắt diff, danh sách cfg key mới, output `-aimtest`, những chỗ còn `[I]`/fallback (offset thiếu,
instance không resolve được).
