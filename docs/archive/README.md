# docs\archive — tài liệu đã xong / đã thay thế

**Không dùng thư mục này làm hướng dẫn hiện tại.** Mọi thứ cần biết đang nằm ở
`AGENTS.md` (cấu trúc + gate), `FINDINGS_overlay_internals.md` (kiến thức đã xác minh),
`HANDOFF_fly_velocity.md` (số đo fly), `HANDOFF_roblox_update_playbook.md` (quy trình update),
`HANDOFF_tools.md` (Tools tab + macro), `HANDOFF_visible_stunts.md` (tab Stunt).

Chỉ mở file ở đây khi cần **lịch sử quyết định** hoặc **số đo gốc** của một refactor đã xong.
Đừng đọc cả thư mục: 18 file, phần lớn là snapshot của code đã bị thay.

## Refactor đã xong (S1 → S3)

| File | Nội dung | Vì sao archive |
|---|---|---|
| `PLAN_S1_split_menu_tabs.md` | kế hoạch tách 8 tab khỏi `overlay.cpp` 7.217 dòng | S1 xong; cấu trúc tab hiện tại ở `AGENTS.md` §Tab index |
| `HANDOFF_S1_split_menu.md` | kết quả S1 (6.123 dòng) | xong |
| `PLAN_S2_cfg_server_world.md` | kế hoạch tách cfg/server/world | S2 xong |
| `HANDOFF_S2_cfg_server_world.md` | kết quả S2 (4.085 dòng) | xong |
| `PLAN_S3_aim_esp_cache.md` | kế hoạch tách cache/ESP/aim (50 KB) | S3 xong; module map ở `AGENTS.md` §Cấu trúc overlay |
| `HANDOFF_S3_aim_esp_cache.md` | kết quả S3 core; ghi chú S3.3b | S3.3b đã xong (`ovl_aim.cpp` + `-aimtest`) |
| `DEAD_CODE_AUDIT.md` | audit dead code trên **monolith cũ** 7.217 dòng | code đã tách sang `ovl\*`; muốn audit lại thì chạy lại, đừng đọc file này |

## Menu / tab đời đầu (trước S1)

| File | Nội dung |
|---|---|
| `menu_handoff.md` | menu ImGui click được + draggable, lưu vị trí vào `overlay.cfg` |
| `menu_v2_handoff.md` | bỏ HUD, checkbox = công tắc arm, hotkey |
| `menu_tabs_handoff.md` | chia tab + tab Teleport, bỏ hotkey H |
| `HANDOFF_menu_collapse.md` | thu gọn panel bằng tam giác (INSERT) |
| `HANDOFF_dot1_8tabs.md` | đợt 1: 8 tab + ESP v2 + radar + profiles + waypoints |
| `HANDOFF_server_tab.md` | đợt 4: Server tab (hop/join/ping) |
| `HANDOFF_world_tab.md` | đợt 2: World tab (lighting/terrain/effects) |
| `PLAN_speed_jump.md` | speed/jump không bị Error 268 — bản gốc; kết luận đã gộp vào `FINDINGS_overlay_internals.md` |

## Prompt build cũ (dùng một lần)

| File | Nội dung |
|---|---|
| `PROMPT_build_phaseC.md` | prompt hoàn thiện nhánh mouse (aim) |
| `PROMPT_build_server_tab.md` | prompt build tab Server |
| `PROMPT_build_world_tab.md` | prompt build tab World |

## Findings velext (RE gốc — còn được trích dẫn)

Khôi phục từ history ngày 06/11/2026 (file đã bị xoá khỏi root nhưng `FINDINGS_overlay_internals.md`
vẫn trích dẫn số hiệu mục của chúng).

| File | Nội dung |
|---|---|
| `velext_aim_findings.md` | RE velext: aim tick, writer, sensitivity (được trích ở §7 mục 2,3,4) |
| `velext_P0_findings.md` | pass 0: bản đồ module velext (được trích ở §7 mục 1) |
| `velext_P1b_findings.md` | pass 1b: runtime chứng minh `Humanoid+0x1CC/+0x3A4` (§7 mục 1) |
| `HANDOFF_aimsmooth.md` | smoothing cũ; kết luận đã gộp vào `FINDINGS_overlay_internals.md` §1 |
| `HANDOFF_speed_jump.md` | speed/jump cũ; kết luận đã gộp vào `FINDINGS_overlay_internals.md` §3 |

## Quy ước khi archive thêm

1. Chỉ chuyển vào đây khi việc trong file **đã xong** hoặc **đã bị file khác thay thế**.
2. Thêm một dòng vào bảng trên: nội dung + lý do.
3. Nếu file còn việc mở (TODO chưa làm) thì **không** archive — gộp phần còn lại vào file đang sống trước.
4. Xoá reference tới file vừa chuyển trong `AGENTS.md` / playbook (đổi thành `docs\archive\<file>`).
