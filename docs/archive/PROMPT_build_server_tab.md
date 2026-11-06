# PROMPT — BUILD tab Server (ext_overlay): Đợt 4 "Server / session / hop"

> Làm tab Server trước Đợt 3 (Body/Movement) theo yêu cầu — đợt này **độc lập hoàn toàn với
> memory writes** nên thứ tự không quan trọng.

Bạn là coding agent trên repo `C:\Users\adminn\ext_project` (Windows / PowerShell / mingw g++).
`ext_overlay.exe` là **external overlay** (ImGui + DX11, `src\overlay.cpp`): chỉ RPM +
NtWriteVirtualMemory, **không inject DLL, không hook, không chạy Lua trong game**. Menu đã có 8
tab (`Body, Combat, Visual, Teleport | World, Server, Tools, Misc`); **tab Server (index 5) hiện
chỉ là placeholder**:

```cpp
} else if (cfgMenuTab == 5) {
    // ---------------- Server: place/job/session ---------------------
    ImGui::BeginChild("##server", ImVec2(0.0f, 230.0f), true);
    markChildArea();
    ImGui::Text("place  %llu", (unsigned long long)place);
    ImGui::TextDisabled("jobId / server IP / region / ping,");
    ImGui::TextDisabled("server hop, rejoin, anti-AFK: Đợt 4.");
    ImGui::EndChild();
}
```

Việc hôm nay: **hiện thực đầy đủ tab này theo `PLAN_more_features.md` §Đợt 4, mục 4.1–4.6 (+4.10
optional).**

**Đặc điểm lớn nhất của đợt này: KHÔNG ghi một byte nào vào memory game.** Toàn bộ là:
read-only RPM (info) + HTTP public API (server list) + mở URI `roblox://` (join) + SendInput nudge
(anti-AFK). Vì vậy rủi ro 268 = 0 và luật §0.2 (write) **không áp dụng** — nhưng mọi luật UI /
hit-test / side-file / kiến trúc external vẫn nguyên.

## Đọc trước (bắt buộc, theo thứ tự)

1. `AGENTS.md` — luật project.
2. `PLAN_more_features.md` — **§0.3** (protocol test), **§0.4** (convention handoff/backup),
   **§1** (dòng `DataModel`, `StatsItem`, `Player` — offset có sẵn), **§2** (lưu ý scroll trong
   tab: hạ tầng `g_menuHits` đang dùng ở tab Teleport), **§5** (ghi chú auto-rejoin + URI
   `roblox://experiences/start?placeId=..&gameInstanceId=..` đã verify).
3. `menu_tabs_handoff.md` — pattern tab / `BeginChild` / `markChildArea` / `hitMark`; vì sao tabs
   là button (hit rect chính xác).
4. `HANDOFF_world_tab.md` — bản gần nhất cùng quy trình (backup, self-test, mục 7b gotcha, cách
   test sandbox). Làm theo đúng format báo cáo đó.
5. `src\overlay.cpp` — placeholder Server; helper `chkInt` / `sliderFloatCfg` / `sliderIntCfg` /
   `upsertCfgKey` / `hitMark` / `markChildArea` / `toast`; khối `loadAimCfg()` + clamp; vòng loop
   chính — nơi gọi `worldTick`/`hitboxTick` (~3400) và ý nghĩa `wantShow`/`continue`; pattern
   `SendInput` đang có (grep `SendInput`); **comment dòng ~3186 "the panel cannot take keyboard
   focus" → lý do KHÔNG dùng `ImGui::InputText` trong đợt này**; chỗ đọc `GameLoaded` (~2901);
   các địa điểm restore khi game đóng (`grep -n "worldRestoreAll"`).
6. `src\offsets.h` + `src\offsets.cpp` — `Offsets` + `fill()`; **`httpGet(host, path)` + style
   parse JSON (`section()` / `strVal()`)** — tái dùng cho API servers list (xem mục 3).
7. `src\sdk.h` — `findChildByClass`, `walkChildren`, `readClass`, `readName`,
   **`readTStringDirect`** (đọc TString SSO/heap — dùng cho JobId/ServerIP), `placeId()`,
   `gameId()`.

## Kiến trúc chốt trước khi code (tránh đi sai hướng)

- **Read-only với game memory**: đợt này không ghi. Không có gì cần restore; vẫn giữ mọi nguyên
  tắc an toàn hiện có (không đọc string dài kiểu byte-by-byte — xem comment `readRun`).
- Tab Server = 3 khối: **(A)** info panel đọc từ memory (~1s/lần), **(B)** server list + hop/rejoin
  qua HTTP public API + URI, **(C)** anti-AFK + auto-rejoin watchdog.
- **UI không nhận bàn phím** (window `WS_EX_NOACTIVATE`): không `ImGui::InputText`. Nhập liệu duy
  nhất = nút **"Paste & Join"** đọc clipboard (`CF_UNICODETEXT`), chấp nhận jobId thuần hoặc link
  dán vào (xem mục 3).
- Overlay **tự thoát theo khi client Roblox bị thay** (hop/rejoin mở client mới → handle chết →
  đường exit hiện có chạy). Đây là hành vi **chấp nhận v1** — nhưng mọi trạng thái cần thiết
  (`srv.lastjob`, `srv.lastplace`) phải được `upsertCfgKey` **TRƯỚC khi** mở URI.
- `tick` mới (`srvTick`) đặt cạnh `worldTick` để chạy **cả khi panel ẩn/HUD tắt** — anti-AFK và
  watchdog bắt buộc sống ngoài UI. Chỉ **main thread được ghi cfg/log**; worker (nếu có) chỉ
  fetch + set state.
- Không thêm dependency: `-lwinhttp -lshell32` đã có trong build; clipboard dùng user32
  (`OpenClipboard`/`GetClipboardData`/`SetClipboardData`). Toolchain là MinGW **POSIX threads** →
  `std::thread` link được (winpthreads có sẵn); nếu vì lý do nào đó link lỗi → `CreateThread`.

## Việc cần làm

### 1) Offsets mới (offline, không cần RE)

Thêm field vào `Offsets` (`offsets.h`) + parse trong `fill()` (`offsets.cpp`), dữ liệu đã có
trong `offsets_cache.json` (version `version-11f155f3450f4c8e` — **chỉ đọc, không sửa tay**):

| nhóm | key (offset) | ghi chú |
|---|---|---|
| DataModel | `JobId` 272 | **TString** (36 ký tự) — đọc bằng `sdk.readTStringDirect(dm + 272)` |
| DataModel | `ServerIP` 1464 | **TString** — dạng `ip`, đôi khi kèm `\|port`/`:port` → hiển thị nguyên văn |
| DataModel | `PlaceVersion` 420 | số (int) |
| DataModel | `CreatorId` 376 | số (int64) |
| StatsItem | `Value` 1280 | cho ping thật (mục 2) |

- `PlaceId` 392 + `GameId` 384 **đã có** (`o.dataModelPlaceId` / `o.dataModelGameId`, dùng
  `sdk.placeId()` / `sdk.gameId()`).
- Đặt tên field theo style hiện có (gợi ý: `dataModelJobId`, `dataModelServerIP`,
  `dataModelPlaceVersion`, `dataModelCreatorId`, `statsItemValue`). Parse theo đúng helper số
  đang dùng trong `fill()` (xem các dòng gán `o.dataModelPlaceId` / `o.dataModelGameId`); section
  mới `section(json, "StatsItem")` nếu cache có.
- Quy ước: key thiếu → field = 0 → feature tự no-op.
- Validate khi đọc: jobId phải khớp `[0-9a-f-]{32,40}` (chuẩn UUID có gạch); ServerIP lấy phần
  trước `|`/`:` làm IP để geo. Read lỗi → giữ giá trị cũ, đừng nhấp nháy "".

### 2) Server info (4.1) + Ping thật (4.2)

State file-scope (kiểu `WorldState`), refresh ~1s trong `srvTick`:

- `place, gameId, placeVersion, creatorId, jobId (full + short 8 ký tự + "…"), serverIP,
  players hiện tại` (đếm từ ent list đã có, bỏ self/NPC), `ping`.
- **Ping thật**: `findChildByClass(dm, "Stats")` → `walkChildren` → item có `readName() ==
  "Data Ping"` → `read(item + o.statsItemValue, ...)` → ms; miss → hiển thị `ping —` và ghi
  `[I]` vào handoff. Cache pointer item; retry ≤1/s.
- **jobId đổi = server đổi**: cập nhật `srv.lastjob`/`srv.lastplace` (upsert), reset
  uptime, clear region cache; đồng thời là điểm chèn cho 4.10 (toasts) nếu làm.
- **Uptime**: ghi rõ "(cửa sổ này)" trên UI — là thời gian kể từ lần đầu overlay thấy jobId hiện
  tại, không phải uptime server thật (không có nguồn).
- **Region (tùy chọn, mặc định TẮT `srv.geo`)**: khi tick → gọi
  `https://ipwho.is/<ip>` (https, free, không key; để endpoint trong 1 hằng số) → lấy
  `country`/`city`; cache theo IP. Đây là HTTP ra ngoài duy nhất ngoài API Roblox — log diag rõ,
  không tự bật, không gửi gì khác ngoài chính IP server.

### 3) Server list + Hop (4.3) + Rejoin / Join (4.4)

- **Fetch**: `GET https://games.roblox.com/v1/games/{placeId}/servers/Public?sortOrder=Asc&excludeFullGames=true&limit=100`
  (URL theo PLAN §4.3). Tái dùng `httpGet()` của `offsets.cpp` (nếu nó đang trong anonymous
  namespace → expose qua `offsets.h` hoặc copy nguyên khối WinHTTP sang overlay, giữ một chỗ);
  **set `WinHttpSetTimeouts` ~4s** + User-Agent riêng. Nếu 401/403/JSON lạ → fallback:
  `https://apis.roblox.com/universes/v1/places/{placeId}/universe` → universeId →
  `https://games.roblox.com/v2/games/{universeId}/servers/Public?...`. Log cái nào ăn + status
  code vào diag; cả hai fail → toast lỗi + giữ list cũ (`[I]` ghi handoff).
- **Parse** theo style `section()`/`strVal()` hiện có: mảng `"data"` các object cần: `id`
  (jobId), `playing`, `maxPlayers`, `ping`, `fps`. Viết helper quét mảng đơn giản (không thêm
  thư viện JSON).
- **Lọc**: bỏ jobId hiện tại; `playing >= srv.hopmin`; nếu `srv.hopfresh` → bỏ server có
  `playing >= maxPlayers`; **sort ping tăng dần**. (Region filter theo server: API không trả
  region → `[I]`, chỉ geo server hiện tại theo mục 2.)
- **UI list**: `BeginChild` con cuộn (copy pattern player-list của tab Teleport — có sẵn hạ tầng
  `g_menuHits`), mỗi dòng = nút chọn: `jobId ngắn  players/max  ping`; dòng chọn highlight kiểu
  profile slot (PushStyleColor). `hitMark()` sau **mọi** dòng — thiếu là click xuyên xuống game.
- **Nút**: `Refresh` | `Join` (dòng chọn) | `Hop` (dòng đầu sau lọc = "tốt nhất") |
  `Rejoin` (jobId hiện tại) | `Paste & Join` (clipboard) | `Copy jobId` | `Copy link`.
  - `Paste & Join` parse clipboard: (a) jobId thuần; (b) `roblox://…gameInstanceId=…&placeId=…`
    (thứ tự param bất kỳ — tìm bằng `find("placeId=")`/`find("gameInstanceId=")`); (c) web link
    `https://www.roblox.com/games/start?placeId=…&gameInstanceId=…`. Thiếu placeId → dùng place
    hiện tại; parse fail → toast "clipboard không phải link/job".
  - `Copy link` tạo `https://www.roblox.com/games/start?placeId=<P>&gameInstanceId=<J>` → clipboard.
- **Fetch không chặn loop mỗi frame**: hai lựa chọn, chọn một và giữ đơn giản —
  (1) fetch đồng bộ **chỉ khi bấm nút** (timeout 4s; panel đứng hình ngắn — chấp nhận, có tiền lệ
  fetch offsets lúc khởi động); hoặc (2) worker `std::thread` + mutex kiểu `g_snapMu` + cờ atomic
  (trạng thái idle/loading/ok/fail). Đừng vừa sync vừa thread.
- **Mở URI**: `ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL)` với
  `roblox://experiences/start?placeId=<P>&gameInstanceId=<J>` (PLAN §5 đã verify format). Trước
  khi mở: upsert `srv.lastjob`/`srv.lastplace`, toast `"đang join <short>…"`, log URI khi `-diag`.
  Fallback `[I]` (để sẵn hằng số, không cần UI): web URL ở trên nếu user báo `roblox://` không ăn.

### 4) Auto-rejoin (4.5) + Anti-AFK (4.6)

- **Auto-rejoin** (`srv.autorejoin`, watchdog trong `srvTick`):
  điều kiện fire: process còn sống && **đã từng** `GameLoaded(1488)==1` (theo dõi cờ riêng) &&
  hiện `GameLoaded==0` && entity count == 0, giữ liên tục ≥5s && `srv.lastjob` không rỗng &&
  **không trong cooldown 60s** sau một join/hop/rejoin thủ công → fire URI **đúng 1 lần** + toast
  + log diag. Chống double-fire: flag chờ jobId mới xuất hiện thì reset. (Case chính = 268 kick
  làm unload game; disconnect thường có UI riêng → `[I]`, không cố bắt.)
- **Anti-AFK** (`srv.antiafk` + `srv.afkmin` phút, 5..15, default 12): đủ interval && game đang
  foreground (`fgPid == m.pid`) && `!typingNow` && không đang aim (`aimOn`) → SendInput nudge
  **chuột tương đối +1px rồi −1px** (`MOUSEEVENTF_MOVE`, không click) — tái dùng pattern SendInput
  có sẵn trong file. Log mỗi lần nudge khi `-diag` (không log mỗi frame).
- **4.10 optional** (chỉ nếu còn thời gian, S): diff tên player list giữa 2 tick → `toast("X vào
  server")` / `toast("Y rời server")`; bỏ qua NPC; tên lấy từ `readName()` (đã có hạ tầng).

### 5) UI (tab Server)

- Thay placeholder trong `} else if (cfgMenuTab == 5) {`; giữ
  `BeginChild("##server", ImVec2(0.0f, 330.0f), true)` + `markChildArea()`. Bố cục gợi ý:
  1. Khối info TextDisabled 3–4 dòng:
     `place … gameId … v…`, `job <short>…  ip …  ping …`,
     `players …  uptime … (cửa sổ này)  region …`,
     (jobId full có thể để dòng riêng + nút Copy).
  2. `Separator` + hàng nút: `Refresh  Join  Hop  Rejoin  Paste & Join  Copy jobId  Copy link`.
  3. List server — `BeginChild` con (cao ~140) theo pattern tab Teleport; có dòng
     `"chưa có list — bấm Refresh"` / `"đang tải…"` / `"lỗi: <code>"`.
  4. `Separator` + hàng option: `srv.hopmin` (slider int 0..50) | `srv.hopfresh` (chk) |
     `srv.geo` (chk) | `srv.autorejoin` (chk) | `srv.antiafk` (chk) + `srv.afkmin` (slider 5..15).
- **`hitMark()` sau MỌI widget** (kể cả mỗi dòng list).
- cfg mới — parse trong `loadAimCfg()` + clamp:

| key | kiểu | clamp/default | ý nghĩa |
|---|---|---|---|
| `srv.antiafk` | 0/1 | 0 | bật anti-AFK |
| `srv.afkmin` | int | 5..15, default 12 | phút giữa 2 nudge |
| `srv.autorejoin` | 0/1 | 0 | watchdog 268 → join lại |
| `srv.hopmin` | int | 0..50, default 5 | lọc tối thiểu player khi hop |
| `srv.hopfresh` | 0/1 | 1 | bỏ server full |
| `srv.geo` | 0/1 | **0** | geo-IP server hiện tại (HTTP ra ngoài) |
| `srv.lastjob` | string | "" | jobId gần nhất (watchdog dùng) |
| `srv.lastplace` | u64 | 0 | place tương ứng |

### 6) Verify

- Build: `build_fast.bat` (đóng `ext_overlay.exe` trước — file lock; overlay là
  single-instance, xem HANDOFF_world_tab.md §7b).
- `ext_overlay.exe -aimtest` → exit 0 (không regression).
- Thêm arg `-srvtest <placeId>` **đặt cạnh `-aimtest` trong `main()` (trước mutex, để chạy được
  khi overlay khác đang mở)** → chạy đúng pipeline fetch + parse + filter + print ra console,
  **không cần game, không UI** → đây là self-test chính của đợt.
- Offline: exe không crash khi không có game; tab hiện "not found"/số 0; mọi nút no-op + toast.
- In-game checklist (user chạy, ghi vào handoff): info khớp thực tế (đối chiếu jobId qua
  join-link/nguồn khác), `Rejoin` hoạt động, hop sang server khác hoạt động (nhớ: overlay tự
  thoát theo client cũ — mở lại sau), anti-AFK có dòng log khi `-diag`, không dấu hiệu 268
  (đợt read-only nên chỉ cần "không có gì lạ").

### 7) Docs + backup

- Backup trước khi sửa: `backup\overlay_SERVER_<yyyyMMdd>.{cpp,exe}` (theo tên đợt trước).
- `HANDOFF_server_tab.md`: việc đã làm, cfg key mới, output `-aimtest` + `-srvtest`, danh sách
  `[I]`/fallback (endpoint nào ăn, region, disconnect, `roblox://` vs web URL, InputText/clipboard),
  checklist in-game, cách dùng tab.

## Ràng buộc

- **Không ghi memory game trong đợt này** — kể cả "cho chắc". Nếu buộc phải ghi thì phải theo
  §0.2 và nêu rõ lý do trong handoff.
- Không inject/hook/string-write/remote/Lua. Không đụng logic aim/ESP/body/teleport; chỉ thêm key
  mới vào parser + clamp + tab Server.
- Không đọc/gửi cookie, token, tài khoản ra ngoài. HTTP chỉ tới: `games.roblox.com`,
  `apis.roblox.com` (public), geo API (mặc định tắt) — và chỉ gửi đúng tham số cần thiết.
- Mọi file mới (nếu có) dùng `exePath("...")` — side-file cạnh exe, **không path tương đối**.
  Log diag prefix `srv`, rate-limit, không spam mỗi frame.
- Không refactor lan man; giữ style comment hiện có (`[P]/[I]`, log kiểu `world ...`).
- Không chạy Roblox/velext trong lúc build/self-test offline.

## Build (copy từ build_fast.bat)

```
g++ -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp lib\libimgui.a -Ithirdparty\imgui-master -Ithirdparty\imgui-master\backends -Isrc -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
```

## Ngoài phạm vi đợt này (ghi 1 dòng vào handoff, không làm)

- 4.7 (player info + avatar) — thuộc tab Teleport.
- 4.8 `ext_multi.exe` (multi-instance mutex) — tool riêng, chạy trước Roblox.
- 4.9 `ext_fps.exe` / FPS cap XML — tool riêng (file `GlobalBasicSettings_13.xml`, sửa khi Roblox đã tắt).

## Báo cáo cuối

Tóm tắt diff, danh sách cfg key mới, output `-aimtest` + `-srvtest`, danh sách điểm `[I]`/fallback
(endpoint nào ăn, region, disconnect, URI fallback, 4.10 làm hay bỏ).
