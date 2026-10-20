# HANDOFF — Server tab (Đợt 4 "Server / session / hop") — 2026-10-20

Trạng thái: **code xong, build sạch, `-aimtest` + `-srvtest` PASS (network OK)**. Các mục cần
**mắt người trong game** nằm ở mục 7 — user chạy. Rollback point:
`backup\overlay_SERVER_20261020.cpp` + `backup\ext_overlay_SERVER_20261020.exe` +
`backup\overlay_SERVER_20261020.cfg` (chụp trước khi sửa tab Server).

Đặc điểm đợt này: **không ghi một byte nào vào memory game**. Chỉ RPM read-only, HTTPS tới
endpoint public, `ShellExecuteW("roblox://…")`, và `SendInput` ±1px cho anti-AFK. Không
inject/hook/Lua/remote; không đụng logic aim/ESP/body/teleport.

---

## 1. Việc đã làm

| File | Nội dung |
|---|---|
| `src\offsets.h` | +5 field Đợt 4: `dataModelJobId` (272), `dataModelServerIP` (1464), `dataModelPlaceVersion` (420), `dataModelCreatorId` (376), `statsItemValue` (1280); khai báo `httpGetEx()` |
| `src\offsets.cpp` | `httpGetEx(host, path, timeoutMs, UA, *status)` là implementation WinHTTP duy nhất; `httpGet` cũ thành wrapper mỏng (timeout 0 = mặc định, UA `ext-p0/1.0`). `fill()` parse 5 key mới; key thiếu → 0 → feature tự no-op. Section `StatsItem` mới |
| `src\overlay.cpp` (cfg) | 8 flag `cfgSrv*` khai báo cạnh `cfgWorld*`, parse + clamp trong `loadAimCfg()` |
| `src\overlay.cpp` (module) | Khối Server đặt giữa `worldTick()` và `buildAimMatrix()`: JSON scanner, clipboard UTF-8/UTF-16, jobId validator, URI builder, worker HTTP, ping reader, filter, `srvTick()`, `runSrvSelfTest()` |
| `src\overlay.cpp` (main) | dispatch `-srvtest <placeId>` cạnh `-aimtest` (trước mutex single-instance); `std::thread srvTh(srvWorkerThread)` cạnh `writerTh`; exit: `g_srvRun=false` + join (worst case 1 request) |
| `src\overlay.cpp` (loop) | `srvTick()` gọi ngay sau `worldTick(...)` (~dòng 3405), **trước** fast-path `wantShow`/`continue` → watchdog + anti-AFK sống cả khi panel ẩn; roster snapshot 1 Hz từ `ents` (bỏ `self`, bỏ `isNPC`) |
| `src\overlay.cpp` (panic) | Panic thêm `cfgSrvAntiAfk = 0; cfgSrvAutoRejoin = 0; cfgSrvGeo = 0;` |
| `src\overlay.cpp` (UI) | Tab 5 thay placeholder: `BeginChild("##server", 0, 330)` + `markChildArea()`, `hitMark()` sau **mọi** widget kể cả từng dòng list; list con `##srvlist` cao 100 |

### Kiến trúc

- **Worker thread** (chọn phương án 2 của spec): `std::thread` + `std::mutex` + `std::atomic`,
  cùng kiểu `g_snapMu`. Worker **chỉ fetch + set state** trong `g_srvWire`; **main thread**
  mới ghi cfg/log/toast. Request list có cờ `g_srvWantList` + place; geo có `g_srvWantGeo`.
  Cancel check giữa các request (`g_srvRun`) → exit trễ tối đa ~1 request (4 s), không phải
  cả pipeline 3 request.
- **Ping**: `findChildByClass(dm,"Stats")` → `walkChildren` → item `readName()=="Data Ping"`
  → `read(item + o.statsItemValue, double)`; nếu ngoài [1,100000] thử `float`. Cache pointer,
  rescan ≤1/s; miss → UI `--` + `[I]` một dòng diag/5 s.
- **jobId đổi = server đổi**: cập nhật `srv.lastjob`/`srv.lastplace` (upsert **trước** khi mở
  URI), reset uptime, clear region/ping cache, reset roster + watchdog.
- **Auto-rejoin**: `wasLoaded` (đã từng `GameLoaded==1`) && `GameLoaded==0` && `ents==0`
  **liên tục 5 s** && `srv.lastjob` không rỗng && ngoài cooldown 60 s → mở URI đúng 1 lần.
  Cửa sổ 5 s là **liên tục**: mẫu nào phá điều kiện là reset timer.
- **Anti-AFK**: đủ `srv.afkmin` && game foreground (`fgPid == m.pid`) && `!typingNow` &&
  `!aimOn` → `SendInput` `MOUSEEVENTF_MOVE` dx=+1 rồi −1 (không click).
- **4.10** (optional): diff roster 1 Hz → toast `X joined the server` / `Y left the server`;
  bỏ self/NPC; bỏ qua lần fill đầu và roster rỗng; tối đa 3 toast mỗi loại/lần sample.

## 2. cfg key mới

| key | kiểu | default / clamp | ý nghĩa |
|---|---|---|---|
| `srv.antiafk` | 0/1 | 0 | bật anti-AFK |
| `srv.afkmin` | int | 12, clamp 5..15 | phút giữa 2 nudge |
| `srv.autorejoin` | 0/1 | 0 | watchdog 268/unload → join lại |
| `srv.hopmin` | int | 5, clamp 0..50 | lọc tối thiểu player khi hop |
| `srv.hopfresh` | 0/1 | 1 | bỏ server full |
| `srv.geo` | 0/1 | **0** | geo-IP server hiện tại (HTTP ra ngoài) |
| `srv.lastjob` | string | "" | jobId gần nhất — overlay tự ghi, watchdog dùng |
| `srv.lastplace` | u64 | 0 | place tương ứng |

Không cần sửa tay `overlay.cfg`: key được `upsertCfgKey` thêm khi đổi trạng thái (giống
`world.*`). `loadAimCfg` clamp đủ; `srv.geo` mặc định **tắt** đúng spec.

## 3. Self-test (output nguyên văn, lần build cuối)

`build_fast.bat` → `build overlay exit: 0`; `ext_overlay.exe` mới **7,775,980 bytes**
(01:25 20/10).

`ext_overlay.exe -aimtest` → **exit 0**:
```
  sens input bounds [1e-3,100]: edges accepted=1, rejected (0, lo/10, hi+1, NaN, inf)=1
  [5] all mapping cases PASS

=== done ===
```

`ext_overlay.exe -srvtest 101770480176177` → **exit 0**:
```
=== ext_overlay server tab self-test (no game) ===
place=101770480176177

[1] jobId validation
  uuid=1 bare=1 empty=1 short=1 badhex=1 PASS
[2] ServerIP -> IP for geo
  1.2.3.4|53640      -> 1.2.3.4    PASS
  1.2.3.4:53640      -> 1.2.3.4    PASS
  1.2.3.4            -> 1.2.3.4    PASS
  2001:db8::1        ->            PASS
[3] clipboard join parser
  bare=1 roblox=1 web=1 reject=1 PASS
  uri  roblox://experiences/start?placeId=123&gameInstanceId=8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456 PASS
[4] server-list JSON parser
  parse=1 rows=3 first_playing=3 second_playing=12 PASS
[5] hop filter
  kept=2 best_ping=40 PASS

[6] network (WARN only - offline is not a failure)
  list: OK endpoint=v1 http=200 servers=100 (547 ms)
    05acfe69  1/12  ping 132
    94ce5994  1/12  ping 37
    e52eac57  1/12  ping 0
  filter(default hopmin=5 fresh=1): 0 of 100
  geo:  OK http=200 Brisbane, Australia

=== done (PASS) ===
```

- Thuần (mục 1–5) là **điều kiện fail**; mục 6 chỉ WARN — offline vẫn exit 0.
- `-srvtest` chạy **trước** mutex single-instance và **không mở process game**, nên chạy
  được khi overlay khác đang mở / Roblox đang chơi.
- `-srvtest` không gọi `loadAimCfg` → dùng default (hopmin=5, fresh=1), đúng như in ra.

## 4. Kết quả endpoint / parse (mạng thật, lúc test)

- **v1 primary ăn**: `GET https://games.roblox.com/v1/games/{placeId}/servers/Public?sortOrder=Asc&excludeFullGames=true&limit=100`
  → HTTP 200, ~0.5 s, 100 dòng. Response **có** `playing`; parser vẫn fallback đếm
  `playerTokens` nếu field này vắng (đã test bằng JSON mẫu: `first_playing=3`).
- Một số dòng trả `ping=0` → sort đẩy xuống cuối (`ping<=0` coi như 999999).
- Fallback chưa kích hoạt trong test (v1 OK) nhưng đã code đủ: `apis.roblox.com/universes/v1/places/{placeId}/universe`
  → `games.roblox.com/v2/games/{universeId}/servers/Public?…`; diag ghi endpoint + status.
- **Geo**: `https://ipwho.is/<ip>` → HTTP 200, `city/country` parse OK (test `1.1.1.1` →
  `Brisbane, Australia`). Chỉ chạy khi `srv.geo=1`; request 1 lần/IP, retry 60 s nếu fail.
- Place test `101770480176177` là game nhỏ (max 12): filter default `hopmin=5` → 0 server,
  đúng logic (không phải lỗi).

## 5. `[I]` / fallback / điểm chưa verify

| # | Điểm | Ghi chú |
|---|---|---|
| 1 | **Ping thật** (`Stats` → `Data Ping` → `StatsItem.Value`) | Chưa verify in-game (phiên này không ghi/không test live). Đọc `double` trước, `float` sau; miss → `--` + diag `srv-ping-miss` / `srv-ping-read`. Nếu game này đặt tên item khác → sửa 1 chuỗi `"Data Ping"` |
| 2 | **Region** | API server list **không trả region** → không lọc server theo region; chỉ geo server hiện tại (opt-in). `[I]` |
| 3 | **Disconnect thường** (UI lỗi của Roblox) | Không cố bắt; watchdog chỉ bắt case `GameLoaded 1→0` + 0 entity ≥5 s. `[I]` |
| 4 | **Process chết hẳn** (kick đóng client / hop mở client mới) | Overlay exit theo client cũ (WaitForSingleObject) — watchdog không cứu được; mở lại overlay. Accepted v1 |
| 5 | **URI `roblox://experiences/start?placeId=..&gameInstanceId=..`** | Theo PLAN §5 (đã verify trước đây), **không test lại** phiên này. Fallback: đổi `kSrvWebUriFallback = true` → dùng `https://www.roblox.com/games/start?…` |
| 6 | **Không có `ImGui::InputText`** | Panel không nhận keyboard focus. Nhập liệu duy nhất = `Paste & Join` (clipboard `CF_UNICODETEXT`), nhận jobId thuần / `roblox://…` / web link, param bất kỳ thứ tự; thiếu `placeId` → place hiện tại |
| 7 | **ASCII `..` thay vì `…`** | Font ImGui mặc định không có U+2026; mọi chữ trong UI giữ ASCII (label/toast tiếng Anh như các tab cũ) |
| 8 | **`playing`** | v1 hiện có `playing`; nếu API đổi, parser đếm `playerTokens` |
| 9 | **4.10 roster diff** | Xấp xỉ client-side: entity list chỉ có tên; player đang stream-in có thể hiện như "joined". Đã giảm nhiễu: bỏ lần fill đầu, bỏ roster rỗng, cap 3 toast/loại. Có thể tắt bằng cách không dùng (không có key riêng) |
| 10 | **Exit delay** | Worker join sau `g_srvRun=false`; cancel check giữa các request → tối đa ~1 request 4 s (list 3 bước hoặc geo) |
| 11 | **`srv.creatorId`** | Đọc và giữ trong state nhưng không in ra UI (panel hẹp); sẵn cho tab khác |
| 12 | **Single-instance** | Như cũ (mutex `ext_overlay_singleton`); `-srvtest` đứng trước mutex |

## 6. Ghi chú thiết kế / an toàn

- **0 byte ghi vào game** trong đợt này: không có `m.write`/`writeBytes` trong module Server.
- HTTP chỉ tới `games.roblox.com`, `apis.roblox.com` (public, không auth) và `ipwho.is`
  (chỉ khi `srv.geo=1`, chỉ gửi đúng IP server đọc được). Không đọc/gửi cookie/token.
- Request list **không** tự chạy khi mở tab: chỉ khi bấm `Refresh`/`Hop` (Hop dùng list đã
  có; nếu list rỗng thì toast nhắc Refresh). Tránh gọi API ngoài ý muốn.
- UI list giữ **list cũ** khi fetch fail (dòng `failed: HTTP … (old list kept)`), đúng spec.
- Panel width 250..380; text giữ ≤ ~46 ký tự/dòng; list dòng `xxxxxxxx  p/m  pingms`.
- `markChildArea()` + `hitMark()` sau **mọi** widget, kể cả từng `Selectable` — thiếu là
  click xuyên xuống game.

## 7. Checklist in-game (user chạy, game foreground)

1. Mở overlay → tab Server: `place/game/v` khớp thực tế; `job xxxxxxxx..` khớp jobId thật
   (đối chiếu bằng `Copy link` dán vào trình duyệt, hoặc nguồn khác).
2. `ip` hiện IP server; `ping` hiện số ms (nếu `--`: xem `aim_diag.log` có
   `srv-ping-miss`/`srv-ping-read` → báo lại tên item).
3. `players` khớp số người trong server (sai lệch nhỏ do chỉ đếm entity đã resolve).
4. Bấm `Copy jobId` → dán ra Notepad đúng UUID. `Copy link` → mở link đúng place/job.
5. `Refresh` → list hiện sau ~1 s (`loading...` → danh sách). Bấm 1 dòng → highlight →
   `Join` → client mở server đó (overlay cũ tự thoát theo client cũ — mở lại overlay).
6. `Hop` → vào server ping thấp nhất trong list đã lọc (kiểm tra lại tab sau khi vào).
7. `Rejoin` → về đúng server hiện tại (dùng để test URI nhanh nhất).
8. `Paste & Join`: copy link web hoặc `roblox://…` hoặc jobId thuần → dán → join đúng.
9. `srv.geo=1` → dòng region hiện `City, Country` (sau ~1 s); tắt lại → region biến mất
   khỏi UI và không còn request ipwho.is (xem diag `srv-geo`).
10. Anti-AFK: bật, đặt 5 phút, đứng yên trong game foreground → sau 5 phút có dòng
    `anti-afk nudge` trong `aim_diag.log` (chạy `-diag`); không thấy click/giật camera.
11. Auto-rejoin: bật, join 1 server, rồi thoát về menu Roblox (không thoát client) → sau
    ~5 s overlay mở lại URI (toast `auto-rejoin …`); nếu client bị đóng hẳn thì overlay
    cũng thoát (mục 5 #4).
12. Panic (DELETE) → anti-AFK/auto-rejoin/geo tắt ngay (checkbox về off), không có gì lạ.
13. Không có dấu hiệu 268/kick khi bật/tắt các mục trong tab này (đợt read-only).

## 8. Cách dùng tab (tóm tắt)

- Info: place/game/version, `job` 8 ký tự + IP, players/uptime (cửa sổ này)/ping,
  region nếu bật geo.
- Hàng nút 1: `Refresh` `Join` (dòng chọn) `Hop` (dòng đầu = ping thấp nhất) `Rejoin`.
- Hàng nút 2: `Paste & Join` `Copy jobId` `Copy link`.
- List: mỗi dòng `jobId ngắn  playing/max  ping`; bấm để chọn.
- Option: `hop min` 0..50, `fresh` (bỏ full), `geo` (HTTP ngoài), `auto-rejoin`,
  `anti-AFK` + phút 5..15.
- Log diag (khi `-diag`): `srv-job`, `srv-list`, `srv-list-v1/v2`, `srv-geo`, `srv-join`,
  `srv-antiafk`, `srv-autorejoin`, `srv-roster`, `srv-ping-*`. Rate-limit, không spam/frame.

## 9. Ngoài phạm vi (không làm, đúng prompt)

- 4.7 player info + avatar (tab Teleport) — chưa làm.
- 4.8 `ext_multi.exe` (multi-instance) — chưa làm.
- 4.9 `ext_fps.exe` / FPS cap XML — chưa làm.
