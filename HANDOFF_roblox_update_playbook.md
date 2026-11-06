# Playbook: Roblox update → tool vẫn chạy

Mục tiêu: sau mỗi lần Roblox update, **AI khác tự làm hết phần automate được**, và
chỉ báo bạn đúng những việc mà chỉ bạn làm được. Bạn không phải dò offset.

Đọc kèm: `AGENTS.md` (cấu trúc project + gate chuẩn). Contract module cũ nằm ở
`docs\archive\HANDOFF_S3_aim_esp_cache.md` — chỉ mở khi cần lịch sử.
File này chỉ nói về **quy trình update**.

---

## 0. Nguyên tắc (đừng phá)

1. **Mọi con số layout lấy từ dump, không hardcode.** Nếu một field có trong
   `offsets.json` thì nó phải được parse vào `Offsets` rồi dùng. Hardcode chỉ còn
   `readRun` 48 byte (cửa sổ quét string, không phải layout).
2. **Không bao giờ chạy với offset = 0.** `validateOffsets()` chặn ở `ensure()`;
   nếu nó reject thì tool không được mở, không được "cố chạy".
3. **Fail loud.** Mọi thứ reject/disable đều in lý do ra console + `aim_diag.log`.
   Không có tính năng nào được phép "tự tắt im lặng".
4. **Version mismatch = read-only.** Không ghi memory khi `offsets version` khác
   `running version`.
5. **Build khi overlay đang chạy = không build.** Script tự chặn (`exit /b 2`).

---

## 1. TL;DR cho AI (đường ngắn nhất)

| # | Việc | Lệnh | Cần game? | Ai làm |
|---|------|------|-----------|--------|
| 1 | Lấy version Roblox đang pin | `powershell -c "(Invoke-WebRequest https://offsets.imtheo.lol/roblox/version -UseBasicParsing).Content"` | không | AI |
| 2 | Build | `build.bat` (full) / `build_fast.bat` (chỉ overlay) | không | AI |
| 3 | Test parser offline | `ext_overlay.exe -offtest offsets_fixtures` | không | AI |
| 4 | Test chain live | `ext_overlay.exe -verify` | **có** | AI chạy, **bạn** mở game |
| 5 | Test 8 tab | sandbox + `-diag -nohide -t 5` (xem §5) | **có** | AI chạy, **bạn** mở game |
| 6 | Smoke test in-game | bật aim/ESP/teleport/fly thật | **có** | **chỉ bạn** |
| 7 | Bless fixture mới | `-offdump` → `.expect` → commit | không | AI |

Nếu bước 4 = `verify: PASS` và bước 5 ra đủ 8 tab có `hits>0`: xong. Không sờ vào code.

---

## 2. Việc AI làm được vs việc chỉ bạn làm được

**AI làm được (không cần hỏi bạn):**
- Build, đọc log, so hash/timestamp exe, phát hiện binary cũ.
- Fetch dump mới, parse, `-offtest`, `-offdump`, so 2 version bằng `tools\offsets_diff.ps1`.
- Chạy `-verify`, `-aimtest`, `-srvtest`; đọc `aim_diag.log`.
- Chạy overlay trong sandbox, đọc hit-test 8 tab, so với baseline.
- Sửa code khi lỗi thuộc về project (parser, offset chưa parse, hardcode còn sót).
- Thêm field vào `Offsets` + `OFF_FIELDS` + `validateOffsets` + fixture.
- Viết báo cáo: bước nào fail, field nào đổi, cần bạn làm gì.

**Chỉ bạn làm được (không thể delegate):**
1. **Mở Roblox + vào một place** (tool cần process đang chạy; AI không tự bấm được
   vào game, và không nên tự ý mở game của bạn).
2. **Smoke test cảm giác thật**: aim có bám đúng đầu không, fly có bay không, ESP
   có lệch không, teleport có teleport đúng chỗ không. Memory đọc đúng ≠ cảm giác đúng.
3. **Quyết định tin dump**: nếu theo ra dump cho version mới nhưng `-verify` FAIL,
   bạn quyết định pin Roblox về version cũ (ExploitStrap) hay chờ theo sửa.
4. **Việc cần quyền/đăng nhập của bạn**: chọn place để test (game có Humanoid,
   nhiều player/NPC), và xác nhận "hành vi trong game có bị kick không".
5. **Quyết định rollback** nếu một thay đổi làm hỏng thứ đang chạy được.

Thời gian bạn phải bỏ ra trong điều kiện bình thường: **mở game + 3–5 phút smoke test**.

---

## 3. Quy trình đầy đủ (AI chạy tuần tự)

### B0 — Xác định 2 version
```powershell
# version Roblox đang pin (ExploitStrap) — cần game đang chạy để đọc từ process
(Get-Process RobloxPlayerBeta).Path
# version theo đang phục vụ
(Invoke-WebRequest https://offsets.imtheo.lol/roblox/version -UseBasicParsing).Content
```
Nếu 2 cái khác nhau: **đừng build xong là tin**. `ensure()` sẽ tự chọn pinned dump
nếu có; nếu không có thì nó dùng newest + log `NOTE`, và overlay sẽ vào **read-only**.
Nói bạn pin Roblox về đúng version của dump (`offsets_*.json` → `"Roblox Version"`).

### B1 — Build
```bat
build_fast.bat        :: chỉ ext_overlay.exe (~1 phút)
build.bat             :: tất cả tool (~3 phút)
```
- Script tự `exit /b 2` nếu `ext_overlay.exe` đang chạy (file bị khoá).
- Cuối build in `ext_overlay.exe <timestamp> sha256=<12 ký tự>`. Ghi lại hash này;
  nó là thứ duy nhất chứng minh "đang chạy đúng bản vừa build".
- Gate: mọi dòng `exit: 0` (full build = 22 tool).

### B2 — Parser offline (không cần game)
```bat
ext_overlay.exe -offtest offsets_fixtures
```
Kỳ vọng:
```
ok   version-....json version=version-...  [optional keys unusable: ...]
offtest: files=1 compared=1 keys=50 fails=0
```
Fail = parser/format đổi → đọc dòng `FAIL`, so với `tools\offsets_diff.ps1`.

Test âm (phải FAIL, chứng minh validator còn sống):
```bat
ext_overlay.exe -offtest offsets_fixtures\negative
```

### B3 — Chain live (cần game mở)
```bat
ext_overlay.exe -verify
```
Kỳ vọng:
```
chain dm=OK ws=OK cam=OK players=OK local=OK model=OK humanoid=OK hrp=OK prim=OK place=101770480176177
verify: PASS (running=version-... offsets=version-... src=live hash=...)
```
- `hash=` là fingerprint của **các field tool dùng** (không phải hash cả file), nên
  2 lần chạy trên cùng layout phải ra cùng hash. Hash khác = có field đổi thật.
- Exit 0 = PASS, 2 = FAIL, 1 = không thấy process.
- `model/hrp/prim=FAIL` ngay sau khi join là bình thường (character chưa stream) —
  chỉ `dm/ws/players/local/place` là bắt buộc.
- FAIL → dòng `chain FAIL at <bước>` chỉ đúng chỗ chết. Xem §6.

### B4 — 8 tab (cần game mở)
Sandbox để cfg/offsets không đụng bản thật:
```powershell
$sb = "$env:TEMP\opencode\ovlsbx"
New-Item -ItemType Directory -Force $sb | Out-Null
Copy-Item ext_overlay.exe,offsets_cache.json,overlay.cfg $sb -Force
foreach ($t in 0..7) {
  $c = [IO.File]::ReadAllText("$sb\overlay.cfg", [Text.Encoding]::UTF8)
  $c = [regex]::Replace($c, '(?m)^menu\.tab=\d+', "menu.tab=$t")
  $c = [regex]::Replace($c, '(?m)^menu\.collapsed=\d+', "menu.collapsed=0")
  [IO.File]::WriteAllText("$sb\overlay.cfg", $c, (New-Object Text.UTF8Encoding($false)))
  (& "$sb\ext_overlay.exe" -diag -nohide -t 5 2>&1) |
    Select-String 'menu tab=' | Select-Object -Last 1
}
```
Kỳ vọng: 8 dòng, `hits` > 0, `size` khác 250x35 (layout thật). Baseline đã đo:
`17 / 15 / 28 / 22 / 25 / 23 / 10 / 32` (thứ tự Body..Misc). Tab 3 (Teleport) và
tab 5 (Server) đổi theo dữ liệu sống (số entity/số server) nên ±1–2 là bình thường;
7 tab còn lại phải khớp chính xác.
`vp=0,0,0,0` khi cửa sổ Roblox bị minimize là bình thường, không phải bug.

### B5 — Xác nhận tầng entity (không cần UI)
```powershell
Get-Content "$sb\aim_diag.log" -Tail 8
```
Kỳ vọng: `entity refresh dm=... players=... local=... cache=>0 ents=>0`,
`srv job=<uuid> place=<id>`, `world lighting=... terrain=...`.
`ents=0` + `cache=0` = tầng name/HRP chết (xem §6).

### B6 — Bless fixture (chỉ khi mọi thứ PASS)
```bat
copy offsets_cache.json offsets_fixtures\version-<new>.json
ext_overlay.exe -offdump offsets_fixtures\version-<new>.json > offsets_fixtures\version-<new>.expect
ext_overlay.exe -offtest offsets_fixtures
```
Fixture = "hợp đồng" của parser: lần sau theo đổi format, `-offtest` đỏ ngay trên
máy bạn, không cần vào game. Giữ cả fixture của version cũ.

### B7 — Smoke test in-game (chỉ bạn)
Tick lần lượt: ESP box/name/HP → aim (mode mouse + memory) → triggerbot →
teleport → speed/jump → fly → World (clock/fog/FOV) → Server (job id, hop).
Báo AI: cái nào sai + triệu chứng. Không cần mô tả offset.

---

## 4. Khi nào cần sửa code (và sửa ở đâu)

| Triệu chứng | Nơi sửa |
|---|---|
| `-offtest` FAIL dù file JSON đúng | `src/offsets.cpp` (parser/fill) |
| `offsets rejected (...)` | field bị reject: kiểm tra key còn tồn tại trong dump; nếu theo đổi tên → sửa map trong `fill()` |
| `chain FAIL at DataModel` | `FakeDataModel.Pointer`/`RealDataModel` (parse) hoặc `SDK::dataModel` |
| `chain FAIL at Players/LocalPlayer` | `Player.LocalPlayer` / `Players` walk (`Instance.*`) |
| `chain OK` nhưng `ents=0` | `Instance.NameContainer`/`Name` hoặc `BasePart.Primitive` |
| ESP có box nhưng lệch | `VisualEngine.ViewMatrix` / `Camera.*` / projection |
| aim bám sai | `Player.*`, `Humanoid.*`, `Primitive.Position` |
| teleport sai chỗ | `Humanoid.HumanoidRootPart` → `BasePart.Primitive` → `Primitive.Position` |
| World tab không ăn | `Lighting.*` / `Atmosphere.*` / `Terrain.*` |
| ping `--` | `StatsItem.Value` (xem §6) |

Thêm field mới = 4 chỗ, theo thứ tự:
1. `Offsets` trong `src/offsets.h` (kèm comment ý nghĩa).
2. `fill()` trong `src/offsets.cpp` (`intIn(dm, "Key")`).
3. `OFF_FIELDS` (cùng file) nếu muốn nó xuất hiện trong `-offdump`/`.expect`.
4. `validateOffsets()` nếu thiếu nó thì tool phải từ chối chạy.

---

## 5. Bảng chẩn đoán nhanh

| Log / hiện tượng | Nghĩa | Hành động |
|---|---|---|
| `... rejected: offsets rejected (FakeDataModel.Pointer=0)` | key bị đổi tên/không còn | `tools\offsets_diff.ps1` 2 dump, xem ADDED/REMOVED |
| `... rejected: JSON parse failed` | dump cụt/không phải JSON | fetch lại; kiểm tra endpoint |
| `WARN: optional keys unusable: StatsItem.Value=52434` | dump có offset rác | không chặn; ping hiện `--` (báo theo) |
| `offsets_diff.ps1` báo `Humanoid.PlatformStatePointer` đổi | pointer sống trong dump, không phải layout | bỏ qua (script đã ignore) |
| `WARN: offsets X != running Y -> READ-ONLY` | pin sai version | pin Roblox đúng version của dump |
| `chain FAIL at ...` | pointer chain chết | xem §4 theo tên bước |
| `chain NOTE: hai DataModel route lệch` | dump lệch 1 trong 2 route | so 2 pointer, báo theo |
| `ents=0` nhưng `chain ... hrp=OK` | cache lọc hết (name/prim) | `-verify` xem `prim`; đọc `entity-reresolve` |
| `vp=0,0,0,0` | cửa sổ game minimize | bình thường |
| `[build] ext_overlay.exe is running` | file exe bị khoá | đóng overlay rồi build lại |
| Linker `Permission denied` | y như trên (script cũ) | `taskkill /IM ext_overlay.exe /F` |
| overlay mở nhưng tab trống, **không** có log nào | binary cũ đang chạy | so `sha256` build với process; chạy lại bản mới |

---

## 6. Ba trường hợp thật sự phải "mò" (và cách giảm thiểu)

1. **theo ngừng publish một field** (field biến mất khỏi JSON). Cách xử lý: lấy
   fixture cũ làm tham chiếu, tự scan trong client (`ext_scanfields`, `ext_explore`,
   `ext_dump` đã có), hoặc mở issue cho theo. Không đoán giá trị.
2. **Dump sai** (ví dụ thật: `StatsItem.Value = 52434`, không align 4, và place
   không có node `Data Ping`). Cách xử lý: `validateOffsets()` đã cảnh báo; coi
   feature đó là `n/a`, đừng "sửa" bằng cách hardcode.
3. **Roblox đổi *cấu trúc*** (không phải offset): ví dụ `Instance.Name` từ
   `container 112 +8` sang `NameContainer 104 +0`. Nếu theo đã publish field mới
   (`NameContainer`) thì chỉ là sửa parse — không phải RE. Nếu theo **không** publish
   field tương ứng thì mới cần RE (đọc `-offdump`, dump neighborhood bằng `ext_explore`).

Ghi chú: hai lỗi đã gặp thật (2026-10) đều **không** phải mò offset:
- key trùng tên trong JSON (`VisualEngine.FakeDataModel` vs top-level) → parser.
- `Instance.Name` đổi layout → dùng `NameContainer` có sẵn trong dump.

---

## 7. Prompt copy-paste cho AI khác

```
Bạn đang ở C:\Users\adminn\ext_project (project overlay Roblox external).
Đọc AGENTS.md và HANDOFF_roblox_update_playbook.md trước.

Nhiệm vụ: sau khi Roblox update, đưa tool về trạng thái chạy được với ít việc
của tôi nhất. Làm tuần tự B0..B5 trong playbook, dừng lại báo tôi khi cần mở game
(B3/B4) hoặc khi cần quyết định (dump lệch version).

Quy tắc bắt buộc:
- Không hardcode offset mới; nếu dump có field thì parse vào Offsets.
- Không bỏ qua validateOffsets / -verify FAIL.
- Không build khi ext_overlay.exe đang chạy (script sẽ chặn, exit 2).
- Mọi kết luận phải kèm output lệnh thật (console hoặc aim_diag.log), không suy đoán.
- Khi xong: báo PASS/FAIL từng bước, hash exe, field nào đổi (offsets_diff), và
  đúng những việc tôi phải làm tay (smoke test nào, ở place nào).
```

---

## 8. Lệnh tham chiếu nhanh

```bat
ext_overlay.exe -aimtest              :: unit test aim math, không cần game, 0 FAIL
ext_overlay.exe -srvtest 0            :: test server list HTTP, không cần game
ext_overlay.exe -verify               :: offsets + chain live, exit 0=PASS 2=FAIL
ext_overlay.exe -offtest [dir]        :: parse fixture + so .expect
ext_overlay.exe -offdump <file.json>  :: in toàn bộ field đã parse (để bless .expect)
ext_overlay.exe -diag -nohide -t 5    :: log hit-test/menu/entity ra stdout + aim_diag.log
powershell -File tools\offsets_diff.ps1 <old.json> <new.json>   :: field nào đổi
```

File sinh ra cạnh exe (không phải CWD): `overlay.cfg`, `offsets_cache.json`,
`offsets_good.json`, `aim_diag.log`, `profiles\`.
