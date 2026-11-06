# PROMPT — BUILD Phase C (ext_overlay): hoàn thiện nhánh Mouse theo dữ liệu Phase B

Bạn là coding agent trên repo `C:\Users\adminn\ext_project` (Windows / PowerShell / mingw g++).
`ext_overlay.exe` (ImGui + DX11, `src\overlay.cpp`) là overlay mô phỏng cảm giác aim của
`velext_v1.exe`. Phase A đã port xong model smoothing. Phase B đã đo xong trong game (read-only).
Việc hôm nay: **build Phase C — offline, KHÔNG cần chạy Roblox/velext.**

## Đọc trước (bắt buộc, theo thứ tự)
1. `AGENTS.md` — luật project.
2. `velext_aim_findings.md` — §5 (công thức angle→counts của velext), §10 (diff với overlay),
   **§14** (Phase B live evidence + corrections). **Chỗ nào §14 mâu thuẫn phần cũ thì tin §14.**
3. `HANDOFF_aimsmooth.md` — trạng thái Phase A + §8 (kết quả Phase B, 4 câu hỏi mở đã có đáp án).
4. Code hiện tại: `src\overlay.cpp`, `src\offsets.cpp` (grep `velext`, `aim.sens`, `gamesens`, `Bp`).

## Việc cần làm

### 1) [CHÍNH] Mode 0 — thay mapping linearized bằng công thức velext thật (Wart #1)
Nhánh mouse hiện dùng gain tuyến tính qua `aim.sens`/`aim.gamesens`. Thay bằng chuỗi thật (§5, đã đo live §14.5):
- Đọc mỗi lần aim: `robloxBase + 0x8D857D8` (float2: **A.x** @+0, **A.y** @+4),
  `robloxBase + 0x8D857E0` (float2 B — chỉ validate), `robloxBase + 0x8D85820` (float C — chỉ validate),
  `MouseService + 0x21C` (float **f**).
- `out1 = fabsf(f) * A.x`, `out2 = A.y * f`; validate cả hai trong [1e-4, 1000].
- Công thức:
  `countsX = atanf((aimScreenX - refX) / focal)/DEG2RAD / max(0.15f, sqrtf(1 - p*p)) / (0.5f * out1) * alpha;`
  `countsY = atanf((aimScreenY - refY) / focal)/DEG2RAD / (0.385f * out2) * alpha;`
  `focal = (h*0.5f)/tanf(fovY*0.5f*DEG2RAD)` (guard 1e-6); `p` = field `cam+0x2C` (§5).
- Nếu `alpha < 1`: `lim = clamp(14000*alpha, 12, 400)`; clamp countsX/countsY vào ±lim.
- **Sự thật Phase B:** giá trị sensitivity slider nằm ở 5 mirror
  `+0x8D857D8/+0x8D857DC/+0x8D857E0/+0x8D857E4/+0x8D85820` (range 0.36..4.0);
  `MouseService+0x21C` giữ 1.0 (hằng scale, không phải slider).
- Tham chiếu cách resolve MouseService: `aim_work\velsample.cpp` (đã chạy thật, read-only).
- Fallback khi đọc lỗi/validate fail: giữ đường `aim.sens`/`aim.gamesens` cũ + 1 dòng log rate-limited.
- Verify nhanh back-pressure hiện có khớp §5 (ngưỡng 0.02°, timeout 60 ms) — chỉ sửa nếu lệch.

### 2) Prediction theo ping (§10, hàng "prediction")
`pred = (pingMs/1000) * predictScale` thay cho clamp 0.25 s. Nếu overlay chưa có nguồn ping →
giữ fallback hiện tại và **ghi rõ trong báo cáo** (không tự bịa thêm dependency).

### 3) Mouse-lock guard cho nhánh SendInput (mới, §14.4)
Nhánh mouse chỉ hiệu quả khi game **mouse-locked** (first-person / shift-lock); ở third-person
free-look inject chỉ làm cursor bay loạn mà camera không quay (operator xác nhận trong game).
Thêm kiểm tra trước khi inject:
- Gợi ý: `GetCursorInfo()` cho foreground window — cursor hidden ⇒ mouse-locked; hoặc dùng
  back-pressure sẵn có: N lần inject liên tiếp mà camera không nhích ⇒ coi như không lock.
- Khi không lock: **skip inject** (đừng spam), log 1 dòng (rate-limited). Nhánh memory-write không bị ràng buộc này.

### 4) Self-test
- `ext_overlay.exe -aimtest` giữ exit 0 + bảng cũ.
- Thêm 2–3 case cho mapping mới (ví dụ A.x=A.y=f=1, alpha=0.32, delta px cho trước → counts tự tính tay để so). Offline.

### 5) Docs
Cập nhật `HANDOFF_aimsmooth.md` (mục "Phase C": việc đã làm, lệnh build, output self-test).
**Không sửa `velext_aim_findings.md`** (hồ sơ RE).

## Ràng buộc
- KHÔNG chạy Roblox / velext / velsample; không ghi vào process khác. Offline + self-test.
- Backup trước khi sửa: `backup\overlay_DOT3_<yyyyMMdd>.{cpp,exe}`.
- Giữ nguyên legacy path `aim.pid=1`, không đổi tên cfg key cũ.
- Style hiện có: comment `[P]/[I]`, log prefix, không refactor lan man.
- Ngoài phạm vi: cursor mode (§5 cuối), Silent method, mọi thứ thuộc velext_v1.exe.

## Build & verify
- `build_fast.bat` (chỉ overlay; lỗi lib thì `build.bat`).
- `ext_overlay.exe -aimtest` → exit 0.
- Báo cáo cuối: tóm tắt diff, lệnh build, output self-test, danh sách điểm còn `[I]`/fallback.
